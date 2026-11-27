using Test, AudioPlugins

@testset "pkg-config paths containing shell metacharacters" begin
    names = Sys.iswindows() ? ["plugin authoring"] :
        ["plugin authoring", "plugin 'authoring", "plugin \"authoring", "plugin \\authoring"]
    mktempdir() do root
        for name in names, quote_char in ("", "'", "\"")
            @testset "$name / $(repr(quote_char))" begin
                dir = mkpath(joinpath(root, name))
                pc = joinpath(dir, "gain.pc")
                prefix = quote_char * "\${prefix}" * quote_char
                pcfiledir = quote_char * "\${pcfiledir}" * quote_char
                write(
                    pc, """
                    prefix=\${pcfiledir}
                    Cflags: -I$prefix/include -I $pcfiledir -DX=1
                    Libs: -L$prefix/lib -lx
                    Libs.private: -L $pcfiledir/private -lm
                    """
                )
                flags = AudioPlugins.pkgconfig_flags(pc)
                @test flags.cflags == ["-I$dir/include", "-I", dir, "-DX=1"]
                @test flags.libs == ["-L$dir/lib", "-lx", "-L", "$dir/private", "-lm"]
            end
        end
    end
end
