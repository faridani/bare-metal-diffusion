sudo snap install rustup
rustup default stable
rustup target add aarch64-unknown-none
rustup component add rust-src


sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu


Build 

```
cargo clean
cargo build --release --target aarch64-unknown-none -vv

```
Run 

```
qemu-system-aarch64 \
  -M virt -cpu cortex-a76 -m 512M \
  -nographic \
  -kernel target/aarch64-unknown-none/release/hello-rust \
  -serial stdio
```