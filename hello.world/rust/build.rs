fn main() {
    println!("cargo:rerun-if-changed=linker.ld");
    println!("cargo:rerun-if-changed=src/boot.S");

    cc::Build::new()
        .compiler("aarch64-linux-gnu-gcc")
        .file("src/boot.S")
        .flag("-xassembler-with-cpp")
        .compile("boot");

    // DO NOT put -Wl, here. Let rustc pass -T directly to the linker.
    println!("cargo:rustc-link-arg=-Tlinker.ld");
}
