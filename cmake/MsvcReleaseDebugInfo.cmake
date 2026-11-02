# ddb_msvc_release_debug_info(<target>)
# MSVC Release builds of <target> keep their debug info in a PDB next to the binary, so a
# crash stack resolves to function names (tests/win_crash_report.cpp, CI crash dumps).
# The binary stays optimized (/OPT:REF /OPT:ICF), and the PDB is not installed or packaged.
function(ddb_msvc_release_debug_info target)
    if(MSVC)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/Z7>)
        target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
    endif()
endfunction()
