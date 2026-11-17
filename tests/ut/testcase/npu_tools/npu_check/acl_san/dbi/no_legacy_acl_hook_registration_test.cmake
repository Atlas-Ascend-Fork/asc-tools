if(NOT DEFINED DBI_ROOT)
  message(FATAL_ERROR "DBI_ROOT is required")
endif()

foreach(removed_file IN ITEMS dbi/acl_hook.h dbi/acl_hook.cpp)
  if(EXISTS "${DBI_ROOT}/${removed_file}")
    message(FATAL_ERROR "DBI legacy ACL hook file must remain deleted: ${removed_file}")
  endif()
endforeach()

file(GLOB dbi_headers "${DBI_ROOT}/dbi/*.h")
file(GLOB dbi_sources "${DBI_ROOT}/dbi/*.cpp")
set(dbi_content "")
foreach(dbi_file IN LISTS dbi_headers dbi_sources)
  file(READ "${dbi_file}" file_content)
  string(APPEND dbi_content "${file_content}\n")
endforeach()

foreach(forbidden_identifier IN ITEMS
    aclrtApiInjectionGetFunc
    aclrtApiInjectionSetFunc
    AclrtBinaryLoadFromDataHook
    InstallAclHooks
    UninstallAclHooks
    NpuCheckInstallAclHooks
    NpuCheckUninstallAclHooks)
  if(dbi_content MATCHES "${forbidden_identifier}")
    message(FATAL_ERROR
      "DBI must not define the legacy ACL hook registration path: ${forbidden_identifier}")
  endif()
endforeach()
