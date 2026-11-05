# CMake before 3.18 can alias only GLOBAL imported targets. Promote the entire
# export together so repeated discovery never sees a partially visible export.
if(CMAKE_VERSION VERSION_LESS 3.18)
    foreach(module can headers can-compat serial modbus)
        if(TARGET linkerbot::${module})
            get_target_property(_linkerbot_global linkerbot::${module} IMPORTED_GLOBAL)
            if(NOT _linkerbot_global)
                set_property(TARGET linkerbot::${module} PROPERTY IMPORTED_GLOBAL TRUE)
            endif()
        endif()
    endforeach()
endif()

# Aliases preserve the archive, dependency surface and exported custom properties.
foreach(pair "comm;can" "comm-values;headers" "comm-compat;can-compat")
    list(GET pair 0 legacy)
    list(GET pair 1 canonical)
    if(NOT TARGET linkerbot::${legacy})
        add_library(linkerbot::${legacy} ALIAS linkerbot::${canonical})
    endif()
endforeach()
