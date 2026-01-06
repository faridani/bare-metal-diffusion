Install the cross compiler 

```bash
sudo apt update
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu
```
Then 

```bash
make clean
make
```

Then run 

```bash
qemu-system-aarch64 \
  -M virt -cpu cortex-a76 -m 512M \
  -nographic \
  -kernel kernel.elf \
  -serial mon:stdio


```