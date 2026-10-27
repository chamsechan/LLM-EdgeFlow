# 在原生发现之后分配标签；这里是元数据，不是覆盖清单。
foreach(case IN LISTS edgeflow_test_core_runner_cases edgeflow_test_nodes_runner_cases test_registry_conflict_cases test_model_backend_registry_conflict_cases test_catalog_contract_ssot_cases)
  set_tests_properties("${case}" PROPERTIES LABELS
    "tier1;dev-fast;sanitizer-compatible;sanitizer-runtime")
endforeach()
foreach(case IN LISTS edgeflow_test_adapter_runner_cases)
  if(case MATCHES "^(BusinessResponsesTest|KeywordResponseTest)\\.")
    set(tier "tier3;integration")
  else()
    set(tier tier2)
  endif()
  set_tests_properties("${case}" PROPERTIES LABELS
    "${tier};dev-fast;sanitizer-compatible;sanitizer-runtime")
endforeach()
foreach(case IN LISTS edgeflow_test_tooling_runner_cases)
  set_tests_properties("${case}" PROPERTIES LABELS
    "tier3;integration;dev-fast;sanitizer-compatible;sanitizer-runtime")
endforeach()
foreach(case IN LISTS edgeflow_test_core_runner_cases test_catalog_contract_ssot_cases edgeflow_test_tooling_runner_cases)
  if(case MATCHES "^(KiteBackendTest|LlamaCppBackendTest|CatalogPublicationTest|SharedDemoConsumerTest)\\.")
    if(case MATCHES "^SharedDemoConsumerTest")
      set(tier "tier3;integration")
    else()
      set(tier tier1)
    endif()
    set_tests_properties("${case}" PROPERTIES LABELS
      "${tier};dev-fast;sanitizer-compatible;sanitizer-runtime;kite")
  endif()
endforeach()
