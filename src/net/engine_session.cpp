#include "engine_session.hpp"

#include "modb/object/object_codec.hpp"

#include <utility>

namespace modb::net {
namespace {

Error make_protocol(std::string message) {
    return Error{ErrorCode::protocol_error, std::move(message)};
}

} // namespace

Result<void> SocketSink::send(const Message& message) {
    const std::scoped_lock lock{write_mu_};
    return send_message(*socket_, message);
}

OpResult execute_op_call(const EngineServices& services, const OpCall& call) {
    OpResult reply{.call_id = call.call_id};
    if (!services.operations) {
        reply.ok = false;
        reply.code = ErrorCode::operation_not_found;
        reply.message = "server has no operation registry";
        return reply;
    }
    // Sem lock do servidor (C11): procs de leitura correm juntas; as de escrita
    // abrem transação, e `Database::begin` as põe uma de cada vez.
    auto outcome = services.operations->dispatch(call.operation_id, call.args, *services.database, &services.caller);
    if (outcome) {
        reply.ok = true;
        reply.payload = std::move(outcome->payload);
    } else {
        reply.ok = false;
        reply.code = outcome.error().code;
        reply.message = outcome.error().message;
    }
    return reply;
}

EngineSession::EngineSession(EngineServices services, SessionSink& sink, Compression codec)
    : services_{std::move(services)}, sink_{&sink}, codec_{codec} {}

EngineSession::~EngineSession() { finish(); }

bool EngineSession::handle_urgent(const Message& message) {
    const auto* cancel = std::get_if<Cancel>(&message);
    if (cancel == nullptr) {
        return false;
    }
    const std::scoped_lock lock{tokens_mu_};
    if (auto found = tokens_.find(cancel->query_id); found != tokens_.end()) {
        found->second.cancel();
    }
    return true;
}

Result<void> EngineSession::handle(const Message& message) {
    if (handle_urgent(message)) {
        return {};
    }

    if (const auto* query = std::get_if<Query>(&message); query != nullptr) {
        if (live_workers_.load(std::memory_order_relaxed) >= static_cast<int>(services_.max_concurrent_streams)) {
            return sink_->send(StreamError{.query_id = query->query_id,
                                           .code = ErrorCode::invalid_argument,
                                           .message = "max concurrent streams exceeded"});
        }
        live_workers_.fetch_add(1, std::memory_order_relaxed);
        std::thread worker{[this, query_copy = *query] {
            run_query(query_copy);
            live_workers_.fetch_sub(1, std::memory_order_relaxed);
        }};
        const std::scoped_lock lock{workers_mu_};
        workers_.push_back(std::move(worker));
        return {};
    }

    if (const auto* list = std::get_if<FacadeList>(&message); list != nullptr) {
        FacadeListOk reply{.request_id = list->request_id};
        if (services_.facades) {
            const auto listed = services_.facades->list();
            reply.facades.assign(listed.begin(), listed.end());
        }
        return sink_->send(reply);
    }

    if (const auto* open = std::get_if<FacadeOpen>(&message); open != nullptr) {
        FacadeOpenOk reply{.request_id = open->request_id,
                           .facade_id = open->facade_id,
                           .facade_version = open->facade_version};
        if (!services_.facades) {
            reply.ok = false;
            reply.code = ErrorCode::facade_not_found;
            reply.message = "server has no facade catalog";
        } else if (auto found = services_.facades->find(open->facade_id, open->facade_version); found) {
            reply.ok = true;
            reply.facade_id = found->facade_id;
            reply.facade_version = found->facade_version;
        } else {
            reply.ok = false;
            reply.code = found.error().code;
            reply.message = found.error().message;
        }
        return sink_->send(reply);
    }

    if (const auto* attach = std::get_if<ShmAttach>(&message); attach != nullptr) {
        return attach_shm(*attach);
    }

    if (const auto* call = std::get_if<OpCall>(&message); call != nullptr) {
        return sink_->send(execute_op_call(services_, *call));
    }

    // Quem autentica é o proxy (ADR-028); o engine responde para o cliente
    // saber que falou direto com ele.
    if (const auto* auth = std::get_if<Authenticate>(&message); auth != nullptr) {
        return sink_->send(AuthenticateOk{.request_id = auth->request_id,
                                          .ok = false,
                                          .code = ErrorCode::invalid_argument,
                                          .message = "the engine does not authenticate; connect through a proxy"});
    }

    return std::unexpected(make_protocol("expected Query, OpCall, or Facade* in session"));
}

void EngineSession::finish() {
    if (finished_) {
        return;
    }
    finished_ = true;
    stop_.store(true, std::memory_order_relaxed);
    if (shm_thread_.joinable()) {
        shm_thread_.join();
    }
    // Cancela consultas ativas para os workers saírem do generator.
    {
        const std::scoped_lock lock{tokens_mu_};
        for (auto& [id, token] : tokens_) {
            (void)id;
            token.cancel();
        }
    }
    // Junta os workers antes de quem é dono do sink fechá-lo (eles ainda
    // podem estar em send).
    const std::scoped_lock lock{workers_mu_};
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

void EngineSession::forget_token(std::uint32_t query_id) {
    {
        const std::scoped_lock lock{tokens_mu_};
        tokens_.erase(query_id);
    }
    sink_->stream_closed(query_id);
}

void EngineSession::run_query(const Query& query) {
    StreamStats stats{};
    query::CancellationToken token;
    {
        const std::scoped_lock lock{tokens_mu_};
        tokens_[query.query_id] = token;
    }
    const auto done = [&] {
        forget_token(query.query_id);
        if (services_.on_stream_stats) {
            services_.on_stream_stats(stats);
        }
    };

    // O próprio `Database` coordena leitores e escritor (C10/C11, ADR-027):
    // `query_objects` segura o lock de leitura só enquanto produz cada objeto,
    // então vários workers leem juntos e uma escrita espera só o passo em
    // curso. O `send`, que pode bloquear, nunca segura o motor.
    if (auto status = sink_->send(StreamBegin{.query_id = query.query_id}); !status) {
        forget_token(query.query_id);
        return;
    }

    object::Database::ObjectQuerySpec spec{
        .type = query.description.type,
        .limit = query.description.limit,
        .project = query.description.project,
        .cancel = token,
        .has_cancel = true,
    };
    if (query.description.equals) {
        spec.equals = std::pair{query.description.equals->field, query.description.equals->value};
    }

    ObjectFrame batch{.query_id = query.query_id, .compression = codec_};

    const auto note_outstanding = [&] {
        const auto outstanding = stats.produced - stats.sent;
        if (outstanding > stats.max_outstanding) {
            stats.max_outstanding = outstanding;
        }
    };

    const auto flush_batch = [&]() -> Result<void> {
        if (batch.records.empty()) {
            return {};
        }
        const auto count = batch.records.size();
        if (auto status = sink_->send_stream_frame(batch); !status) {
            return status;
        }
        stats.sent += count;
        note_outstanding();
        batch.records.clear();
        return {};
    };

    const auto fail_stream = [&](ErrorCode code, std::string message) {
        (void)flush_batch();
        (void)sink_->send(StreamError{.query_id = query.query_id, .code = code, .message = std::move(message)});
        done();
    };

    for (auto& item : services_.database->query_objects(std::move(spec))) {
        if (token.cancelled()) {
            break;
        }
        if (!item) {
            fail_stream(item.error().code, item.error().message);
            return;
        }
        if (services_.fail_after && stats.produced >= *services_.fail_after) {
            fail_stream(ErrorCode::io_error, "injected stream failure");
            return;
        }

        auto payload = object::encode_object_payload(item->fields);
        if (!payload) {
            fail_stream(payload.error().code, payload.error().message);
            return;
        }

        batch.records.push_back(ObjectEnvelope{
            .object_id = item->id,
            .type_definition_id = item->type,
            .payload = std::move(*payload),
        });
        ++stats.produced;
        note_outstanding();

        if (batch.records.size() >= max_in_flight_objects) {
            if (auto status = flush_batch(); !status) {
                forget_token(query.query_id);
                return;
            }
        }
    }

    if (auto status = flush_batch(); !status) {
        forget_token(query.query_id);
        return;
    }
    // Cancel ou fim natural: StreamEnd com o total produzido (conexão reutilizável).
    if (auto status = sink_->send(StreamEnd{.query_id = query.query_id, .total = stats.produced}); !status) {
        forget_token(query.query_id);
        return;
    }
    done();
}

Result<void> EngineSession::attach_shm(const ShmAttach& attach) {
    ShmAttachOk reply{.request_id = attach.request_id};
    Result<shm::Region> region = std::unexpected(Error{ErrorCode::invalid_argument, "session already has a ring"});
    if (!services_.allow_shm) {
        region = std::unexpected(Error{ErrorCode::invalid_argument, "shared-memory ring is served by the proxy"});
    } else if (!shm_region_) {
        region = shm::Region::create(attach.ring_bytes);
    }
    if (!region) {
        reply.ok = false;
        reply.code = region.error().code;
        reply.message = region.error().message;
    } else {
        shm_region_ = std::make_unique<shm::Region>(std::move(*region));
        reply.kind = shm_region_->kind();
        reply.name = shm_region_->name();
        reply.ring_bytes = shm_region_->ring_bytes();
        shm_thread_ = std::thread{[this, r = shm_region_.get()] { serve_shm(*r); }};
    }
    return sink_->send(reply);
}

void EngineSession::serve_shm(shm::Region& region) {
    auto requests = shm::Ring::requests(region);
    auto responses = shm::Ring::responses(region);
    shm::Backoff idle;
    const auto stopping = [&] {
        return stop_.load(std::memory_order_relaxed) ||
               (services_.server_stop != nullptr && services_.server_stop->load(std::memory_order_relaxed)) ||
               region.load_state(shm::k_off_client_state) == static_cast<std::uint32_t>(shm::ClientState::leaving);
    };
    while (!stopping()) {
        // O cliente já mapeou: o nome pode sair do sistema de arquivos.
        if (region.load_state(shm::k_off_client_state) == static_cast<std::uint32_t>(shm::ClientState::attached)) {
            region.unlink();
        }
        auto next = requests.peek();
        if (!next) {
            break;  // anel corrompido: encerra o anel (a sessão segue)
        }
        if (!next->has_value()) {
            idle.wait();
            continue;
        }
        idle.reset();
        auto message = decode_message(**next);
        OpResult reply;
        if (!message) {
            reply = OpResult{.ok = false, .code = message.error().code, .message = message.error().message};
        } else if (const auto* call = std::get_if<OpCall>(&*message); call != nullptr) {
            reply = execute_op_call(services_, *call);
        } else {
            reply = OpResult{.ok = false,
                             .code = ErrorCode::protocol_error,
                             .message = "only OpCall travels over the shared-memory ring"};
        }
        requests.pop();
        auto bytes = encode_message(reply);
        if (bytes && bytes->size() + 7 > responses.capacity()) {
            bytes = encode_message(OpResult{.call_id = reply.call_id,
                                            .ok = false,
                                            .code = ErrorCode::value_too_large,
                                            .message = "result does not fit the shared-memory ring; call over TCP "
                                                       "or attach a larger ring"});
        }
        if (!bytes) {
            break;
        }
        shm::Backoff full;
        for (;;) {
            auto written = responses.try_write(*bytes);
            if (!written || *written || stopping()) {
                break;
            }
            full.wait();  // cliente atrasado em consumir as respostas
        }
    }
    region.store_state(shm::k_off_server_state, static_cast<std::uint32_t>(shm::ServerState::closed));
}

} // namespace modb::net
