cmake_minimum_required(VERSION 3.25)

foreach(_required SOURCE_DIR INSTALL_PREFIX EXPECTED_TAG EXPECTED_SOURCE_REVISION)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} is required")
  endif()
endforeach()

if(NOT EXPECTED_TAG MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "Release tag is not an exact vMAJOR.MINOR.PATCH value: ${EXPECTED_TAG}")
endif()
string(SUBSTRING "${EXPECTED_TAG}" 1 -1 _expected_version)
string(TOLOWER "${EXPECTED_SOURCE_REVISION}" _expected_source_revision)
string(LENGTH "${_expected_source_revision}" _expected_source_revision_length)
if(NOT _expected_source_revision MATCHES "^[0-9a-f]+$" OR
   (NOT _expected_source_revision_length EQUAL 40 AND
    NOT _expected_source_revision_length EQUAL 64))
  message(FATAL_ERROR
    "EXPECTED_SOURCE_REVISION must be a full 40- or 64-digit hexadecimal revision")
endif()

set(_installed_doc_dir "${INSTALL_PREFIX}/share/doc/tekla_db1_sdk")
foreach(_document LICENSE README.md CHANGELOG.md THIRD_PARTY_NOTICES.md)
  set(_source "${SOURCE_DIR}/${_document}")
  set(_installed "${_installed_doc_dir}/${_document}")
  if(NOT EXISTS "${_source}")
    message(FATAL_ERROR "Release source is missing ${_document}")
  endif()
  if(NOT EXISTS "${_installed}")
    message(FATAL_ERROR "Installed release is missing ${_document}")
  endif()
  file(SHA256 "${_source}" _source_sha256)
  file(SHA256 "${_installed}" _installed_sha256)
  if(NOT _source_sha256 STREQUAL _installed_sha256)
    message(FATAL_ERROR "Installed ${_document} differs from the release source")
  endif()
endforeach()

set(_version_header "${INSTALL_PREFIX}/include/tekla/db1/version.hpp")
if(NOT EXISTS "${_version_header}")
  message(FATAL_ERROR "Installed release is missing tekla/db1/version.hpp")
endif()
file(READ "${_version_header}" _version_contents)
string(FIND "${_version_contents}" "version = \"${_expected_version}\";" _version_match)
if(_version_match EQUAL -1)
  message(FATAL_ERROR
    "Installed SDK version does not match ${EXPECTED_TAG}: ${_version_header}")
endif()
string(FIND
  "${_version_contents}"
  "source_revision = \"${_expected_source_revision}\";"
  _source_revision_match)
if(_source_revision_match EQUAL -1)
  message(FATAL_ERROR
    "Installed SDK source revision does not match ${EXPECTED_SOURCE_REVISION}: ${_version_header}")
endif()

set(_package_config "${INSTALL_PREFIX}/lib/cmake/tekla_db1/tekla_db1Config.cmake")
if(NOT EXISTS "${_package_config}")
  message(FATAL_ERROR "Installed release is missing tekla_db1Config.cmake")
endif()
file(READ "${_package_config}" _package_config_contents)
string(FIND
  "${_package_config_contents}"
  "set(tekla_db1_SOURCE_REVISION \"${_expected_source_revision}\")"
  _package_source_revision_match)
if(_package_source_revision_match EQUAL -1)
  message(FATAL_ERROR "Installed CMake package source revision does not match")
endif()

set(_package_version "${INSTALL_PREFIX}/lib/cmake/tekla_db1/tekla_db1ConfigVersion.cmake")
if(NOT EXISTS "${_package_version}")
  message(FATAL_ERROR "Installed release is missing tekla_db1ConfigVersion.cmake")
endif()
file(READ "${_package_version}" _package_version_contents)
string(FIND
  "${_package_version_contents}"
  "set(PACKAGE_VERSION \"${_expected_version}\")"
  _package_version_match)
if(_package_version_match EQUAL -1)
  message(FATAL_ERROR "Installed CMake package version does not match ${EXPECTED_TAG}")
endif()

message(STATUS "Verified installed release artifacts for ${EXPECTED_TAG}")
