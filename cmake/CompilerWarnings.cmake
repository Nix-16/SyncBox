function(syncbox_set_warnings target)
  if(MSVC)
    # /bigobj: template-heavy translation units (WS sessions) exceed the
    # default COFF section limit on MSVC.
    target_compile_options(${target} PRIVATE /W4 /permissive- /bigobj)
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wnon-virtual-dtor
      -Wold-style-cast)
  endif()

  if(SYNCBOX_WARNINGS_AS_ERRORS)
    if(MSVC)
      target_compile_options(${target} PRIVATE /WX)
    else()
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
