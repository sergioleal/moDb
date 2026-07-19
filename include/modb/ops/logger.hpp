#pragma once

// Logger mínimo exposto ao domínio via ExecutionContext (Fase 9).
// Não há I/O de arquivo aqui — o operador/servidor pode plugar um sink.

#include <string>
#include <string_view>
#include <vector>

namespace modb::ops {

class Logger {
public:
    virtual ~Logger() = default;
    virtual void info(std::string_view message) = 0;
    virtual void warn(std::string_view message) = 0;
    virtual void error(std::string_view message) = 0;
};

// Acumula mensagens em memória (testes e demos).
class MemoryLogger final : public Logger {
public:
    void info(std::string_view message) override { info_.emplace_back(message); }
    void warn(std::string_view message) override { warn_.emplace_back(message); }
    void error(std::string_view message) override { error_.emplace_back(message); }

    [[nodiscard]] const std::vector<std::string>& info_messages() const noexcept { return info_; }
    [[nodiscard]] const std::vector<std::string>& warn_messages() const noexcept { return warn_; }
    [[nodiscard]] const std::vector<std::string>& error_messages() const noexcept { return error_; }

private:
    std::vector<std::string> info_;
    std::vector<std::string> warn_;
    std::vector<std::string> error_;
};

class NullLogger final : public Logger {
public:
    void info(std::string_view) override {}
    void warn(std::string_view) override {}
    void error(std::string_view) override {}
};

} // namespace modb::ops
