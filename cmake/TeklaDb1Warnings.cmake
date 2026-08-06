function(tekla_db1_set_project_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(TEKLA_DB1_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wshadow
      -Wsign-conversion
      -ffp-contract=off)
    if(TEKLA_DB1_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

function(tekla_db1_enable_sanitizers target)
  if(NOT TEKLA_DB1_ENABLE_SANITIZERS)
    return()
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /fsanitize=address)
  else()
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined
                                             -fno-omit-frame-pointer)
    # Static libraries do not perform a link step. Propagate the sanitizer
    # runtime requirement to executables and shared libraries that consume them.
    target_link_options(${target} PUBLIC -fsanitize=address,undefined)
  endif()
endfunction()
