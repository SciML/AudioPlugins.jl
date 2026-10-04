# Source-only build used by the live tests. JLLs supply build dependencies; no
# installed host is replaced and the SDK remains a test-only dependency.
using vst3sdk_jll
function build_live(dir)
    root = dirname(@__DIR__)
    lilv = AudioPlugins.LV2Host_jll.Lilv_jll
    sdk = joinpath(vst3sdk_jll.artifact_dir, "include", "vst3sdk")
    sdklib = joinpath(vst3sdk_jll.artifact_dir, "lib", "vst3sdk")
    cc, cxx = AudioPlugins._c_compiler(), AudioPlugins._cxx_compiler()
    generator = Sys.iswindows() ? ["-G", "MinGW Makefiles"] : String[]
    flags = join(AudioPlugins._c_arch_flags(), " ")
    run(`cmake -S $root/csrc -B $dir $generator -DCMAKE_BUILD_TYPE=Release
         -DCMAKE_C_COMPILER=$cc -DCMAKE_CXX_COMPILER=$cxx
         -DCMAKE_C_FLAGS=$flags -DCMAKE_CXX_FLAGS=$flags
         -DLILV_INCLUDE_DIR=$(joinpath(lilv.artifact_dir, "include", "lilv-0"))
         -DLILV_LIBRARY=$(lilv.liblilv_path)
         -DVST3_SDK_ROOT=$sdk -DVST3_SDK_LIBDIR=$sdklib`)
    run(`cmake --build $dir --parallel 2`)
    library = joinpath(dir, (Sys.iswindows() ? "" : "lib") * "audioplugins_live." * Libdl.dlext)
    # MinGW normally prefixes shared libraries with lib as well.
    if Sys.iswindows() && !isfile(library)
        library = joinpath(dir, "libaudioplugins_live.dll")
    end
    return library, (sdk, sdklib)
end
