include_guard(GLOBAL)

function(libtmux_copy_mingw_runtime target)
  if(NOT MINGW)
    return()
  endif()

  set(runtime_names
      libstdc++-6.dll
      libgcc_s_seh-1.dll
      libgcc_s_sjlj-1.dll
      libgcc_s_dw2-1.dll
      libwinpthread-1.dll
      libc++.dll
      libc++abi.dll
      libunwind.dll)

  foreach(runtime_name IN LISTS runtime_names)
    execute_process(
      COMMAND "${CMAKE_CXX_COMPILER}" "-print-file-name=${runtime_name}"
      RESULT_VARIABLE runtime_result
      OUTPUT_VARIABLE runtime_path
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(runtime_result EQUAL 0 AND NOT runtime_path STREQUAL runtime_name AND
       EXISTS "${runtime_path}")
      add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${runtime_path}"
                "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
    endif()
  endforeach()
endfunction()
