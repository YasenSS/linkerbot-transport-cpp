# Imported interface targets retain the old link surface without importing CAN twice.
foreach(pair "comm;can" "comm-values;headers" "comm-compat;can-compat")
    list(GET pair 0 legacy)
    list(GET pair 1 canonical)
    if(NOT TARGET linkerbot::${legacy})
        add_library(linkerbot::${legacy} INTERFACE IMPORTED)
        set_property(TARGET linkerbot::${legacy} PROPERTY
            INTERFACE_LINK_LIBRARIES linkerbot::${canonical})
    endif()
endforeach()
foreach(property VERSION SOURCE_REVISION)
    get_target_property(value linkerbot::can LINKERBOT_TRANSPORT_${property})
    set_property(TARGET linkerbot::comm PROPERTY LINKERBOT_COMM_${property} "${value}")
endforeach()
