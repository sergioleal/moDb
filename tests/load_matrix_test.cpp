#include "test_support.hpp"

#include "matrix.hpp"
#include "profiles.hpp"

using namespace modb::loadtest;

namespace {

void test_case_id_no_variant(TestSuite& suite) {
    Case c;
    c.workload = "create_only";
    c.target = "embedded";
    c.scale = "100k";
    suite.check(c.case_id() == "load.create_only.embedded.100k",
               "case_id sem variante deve ser load.<workload>.<target>.<scale>");
}

void test_case_id_with_variant(TestSuite& suite) {
    Case c;
    c.workload = "create_only";
    c.target = "embedded";
    c.scale = "100k";
    c.concurrency = 16;
    suite.check(c.case_id() == "load.create_only.embedded.100k.c16",
               "concurrency != 1 deve produzir sufixo .c16");

    Case c2;
    c2.workload = "create_only";
    c2.target = "embedded";
    c2.scale = "100k";
    c2.payload = "fat";
    suite.check(c2.case_id() == "load.create_only.embedded.100k.payload_fat",
               "payload != normal deve produzir sufixo .payload_fat");
}

void test_parse_case_id_roundtrip(TestSuite& suite) {
    Case parsed;
    std::string error;
    const bool ok = parse_case_id("load.create_only.embedded.100k", parsed, error);
    suite.check(ok, "parse_case_id deve aceitar um id válido");
    suite.check(parsed.workload == "create_only" && parsed.target == "embedded" &&
                   parsed.scale == "100k" && parsed.objects == 100'000,
               "parse_case_id deve resolver workload/target/scale/objects corretamente");
    suite.check(parsed.case_id() == "load.create_only.embedded.100k",
               "case_id() do resultado de parse_case_id deve reproduzir o id original");
}

void test_parse_case_id_unknown_workload(TestSuite& suite) {
    Case parsed;
    std::string error;
    const bool ok = parse_case_id("load.workload_inexistente.embedded.100k", parsed, error);
    suite.check(!ok, "parse_case_id deve rejeitar workload desconhecido");
    suite.check(!error.empty(), "parse_case_id deve explicar por que rejeitou");
}

void test_expand_matrix_intersection(TestSuite& suite) {
    auto profile = find_profile("load-smoke");
    suite.check(profile.has_value(), "load-smoke deve existir no catálogo de perfis");
    if (!profile) {
        return;
    }
    suite.check(!profile->cases.empty(), "load-smoke não deveria ter conjunto vazio");

    MatrixSelectors selectors;
    selectors.workload = {"create_only"};
    auto expanded = expand_matrix(profile->cases, selectors);
    suite.check(expanded.error.empty(), "restringir por workload conhecido não deve falhar");
    suite.check(expanded.cases.size() == 1,
               "interseção com --workload create_only deve deixar exatamente 1 caso em load-smoke");
    if (expanded.cases.size() == 1) {
        suite.check(expanded.cases.front().workload == "create_only",
                   "o único caso restante deve ser create_only");
    }
}

void test_expand_matrix_unknown_selector_fails(TestSuite& suite) {
    auto profile = find_profile("load-smoke");
    if (!profile) {
        suite.check(false, "load-smoke deveria existir para este teste");
        return;
    }
    MatrixSelectors selectors;
    selectors.workload = {"workload_que_nao_existe"};
    auto expanded = expand_matrix(profile->cases, selectors);
    suite.check(!expanded.error.empty(),
               "--workload com id desconhecido deve falhar, não produzir conjunto vazio silencioso");
    suite.check(expanded.cases.empty(), "nenhum caso deve sobreviver quando o seletor é inválido");
}

void test_expand_matrix_empty_set_is_error(TestSuite& suite) {
    auto profile = find_profile("load-smoke");
    if (!profile) {
        suite.check(false, "load-smoke deveria existir para este teste");
        return;
    }
    MatrixSelectors selectors;
    selectors.target = {"loopback"}; // load-smoke só tem casos embedded
    auto expanded = expand_matrix(profile->cases, selectors);
    suite.check(!expanded.error.empty(),
               "interseção que esvazia o conjunto deve ser erro explícito (§6.1 regra 5), nunca "
               "\"0 casos com sucesso\" silencioso");
    suite.check(expanded.cases.empty(), "conjunto deve ficar vazio quando o erro é reportado");
}

void test_expand_matrix_case_overrides_profile(TestSuite& suite) {
    auto profile = find_profile("load-smoke");
    if (!profile) {
        suite.check(false, "load-smoke deveria existir para este teste");
        return;
    }
    MatrixSelectors selectors;
    selectors.case_ids = {"load.crud_full.embedded.1M"}; // fora do conjunto de load-smoke
    auto expanded = expand_matrix(profile->cases, selectors);
    suite.check(expanded.error.empty(), "--case explícito não deve ser filtrado pelo perfil");
    suite.check(expanded.cases.size() == 1 && expanded.cases.front().case_id() ==
                                                  "load.crud_full.embedded.1M",
               "--case deve substituir o perfil por completo (§6.1)");
}

void test_expand_matrix_environment_cross_product(TestSuite& suite) {
    std::vector<Case> cases(1);
    cases[0].workload = "create_only";
    cases[0].target = "embedded";
    cases[0].scale = "10k";
    cases[0].objects = 10'000;

    MatrixSelectors selectors;
    selectors.environment = {"desktop-windows", "linux-remoto"};
    auto expanded = expand_matrix(cases, selectors);
    suite.check(expanded.error.empty(), "cross-product de ambiente não deve falhar");
    suite.check(expanded.cases.size() == 2,
               "2 ambientes para 1 caso base devem produzir 2 casos (cross-product, não override)");
}

// Dívida D1 (docs-process/PLANO_IMPLEMENTACAO_CARGA.md §2): case_id não pode
// prometer uma dimensão secundária que o runtime ignora.
void test_expand_matrix_rejects_unimplemented_concurrency(TestSuite& suite) {
    std::vector<Case> cases(1);
    cases[0].workload = "create_only";
    cases[0].target = "embedded";
    cases[0].scale = "10k";
    cases[0].objects = 10'000;

    MatrixSelectors selectors;
    selectors.concurrency = {"16"};
    auto expanded = expand_matrix(cases, selectors);
    suite.check(!expanded.error.empty(),
               "--concurrency 16 deve falhar em vez de gerar um case_id que o runtime ignora");
    suite.check(expanded.error.find("Subfase M") != std::string::npos,
               "a mensagem de erro deve citar a subfase que implementará concorrência");
    suite.check(expanded.cases.empty(), "nenhum caso deve sobreviver quando a dimensão é rejeitada");
}

void test_expand_matrix_accepts_implemented_payload(TestSuite& suite) {
    std::vector<Case> cases(1);
    cases[0].workload = "create_only";
    cases[0].target = "embedded";
    cases[0].scale = "10k";
    cases[0].objects = 10'000;

    MatrixSelectors selectors;
    selectors.payload = {"fat"};
    auto expanded = expand_matrix(cases, selectors);
    suite.check(expanded.error.empty(), "--payload fat deve continuar funcionando (dimensão honesta)");
    suite.check(expanded.cases.size() == 1 && expanded.cases.front().payload == "fat",
               "o caso resultante deve carregar payload=fat");
}

void test_unimplemented_dimension_reason_direct(TestSuite& suite) {
    Case ok;
    ok.workload = "create_only";
    ok.target = "embedded";
    ok.scale = "10k";
    suite.check(unimplemented_dimension_reason(ok).empty(),
               "case com todas as dimensões no padrão não deve ser rejeitado");

    Case bad_cache = ok;
    bad_cache.cache = "oversubscribed";
    suite.check(!unimplemented_dimension_reason(bad_cache).empty(),
               "cache não padrão deve ser rejeitado (sem dispatch implementado)");

    Case bad_variant = ok;
    bad_variant.explicit_variant = "c16";
    suite.check(!unimplemented_dimension_reason(bad_variant).empty(),
               "variante explícita de --case não decodificada deve ser rejeitada");
}

void test_expand_matrix_repeat(TestSuite& suite) {
    std::vector<Case> cases(1);
    cases[0].workload = "create_only";
    cases[0].target = "embedded";
    cases[0].scale = "10k";
    cases[0].objects = 10'000;

    MatrixSelectors selectors;
    selectors.repeat = 3;
    auto expanded = expand_matrix(cases, selectors);
    suite.check(expanded.cases.size() == 3, "--repeat 3 deve produzir 3 execuções do mesmo caso");
    if (expanded.cases.size() == 3) {
        suite.check(expanded.cases[0].repeat_index == 0 && expanded.cases[1].repeat_index == 1 &&
                       expanded.cases[2].repeat_index == 2,
                   "repeat_index deve ser 0, 1, 2 nas três repetições");
    }
}

void test_expand_matrix_filter_and_exclude(TestSuite& suite) {
    std::vector<Case> cases(2);
    cases[0].workload = "create_only";
    cases[0].target = "embedded";
    cases[0].scale = "10k";
    cases[1].workload = "crud_full";
    cases[1].target = "embedded";
    cases[1].scale = "10k";

    MatrixSelectors filter_only;
    filter_only.filter = "create_only";
    auto filtered = expand_matrix(cases, filter_only);
    suite.check(filtered.cases.size() == 1 && filtered.cases.front().workload == "create_only",
               "--filter deve restringir por substring no case_id");

    MatrixSelectors exclude_only;
    exclude_only.exclude = {"crud_full"};
    auto excluded = expand_matrix(cases, exclude_only);
    suite.check(excluded.cases.size() == 1 && excluded.cases.front().workload == "create_only",
               "--exclude deve subtrair por substring no case_id");
}

void test_known_catalogs(TestSuite& suite) {
    suite.check(is_known_scale("100k") && !is_known_scale("42k"), "catálogo de escalas");
    suite.check(is_known_workload("crud_full") && !is_known_workload("workload_fantasma"),
               "catálogo de workloads");
    suite.check(is_known_target("embedded") && !is_known_target("alvo_fantasma"),
               "catálogo de alvos");
    suite.check(is_workload_implemented("create_only"),
               "create_only deve estar marcado como implementado (Subfase B)");
    suite.check(is_workload_implemented("create_delete_forward") &&
                   is_workload_implemented("create_delete_reverse") &&
                   is_workload_implemented("create_delete_interleaved"),
               "a escada create_delete_* deve estar marcada como implementada (Subfase D)");
    suite.check(is_workload_implemented("crud_full"),
               "crud_full deve estar marcado como implementado (Subfase E)");
    suite.check(!is_workload_implemented("read_hotspot"),
               "read_hotspot (§4.2.1) ainda não deveria estar marcado como implementado");
    suite.check(is_target_implemented("embedded") && is_target_implemented("loopback") &&
                   is_target_implemented("remote_colocated"),
               "embedded/loopback/remote_colocated devem estar marcados como implementados "
               "(Subfases B/G/I)");
    suite.check(!is_target_implemented("remote_client_local"),
               "remote_client_local ainda não deveria estar marcado como implementado (Subfase I: "
               "sem host remoto disponível para verificar)");
}

void test_all_profiles_listable(TestSuite& suite) {
    for (const auto& name : list_profile_names()) {
        auto profile = find_profile(name);
        suite.check(profile.has_value(), "perfil listado deve ser encontrável: " + name);
    }
    suite.check(!find_profile("perfil-que-nao-existe").has_value(),
               "perfil desconhecido deve devolver nullopt");
}

} // namespace

int main() {
    TestSuite suite;
    test_case_id_no_variant(suite);
    test_case_id_with_variant(suite);
    test_parse_case_id_roundtrip(suite);
    test_parse_case_id_unknown_workload(suite);
    test_expand_matrix_intersection(suite);
    test_expand_matrix_unknown_selector_fails(suite);
    test_expand_matrix_empty_set_is_error(suite);
    test_expand_matrix_case_overrides_profile(suite);
    test_expand_matrix_environment_cross_product(suite);
    test_expand_matrix_rejects_unimplemented_concurrency(suite);
    test_expand_matrix_accepts_implemented_payload(suite);
    test_unimplemented_dimension_reason_direct(suite);
    test_expand_matrix_repeat(suite);
    test_expand_matrix_filter_and_exclude(suite);
    test_known_catalogs(suite);
    test_all_profiles_listable(suite);
    return suite.finish();
}
