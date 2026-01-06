#![no_std]
#![no_main]

use core::panic::PanicInfo;

const UART0_BASE: usize = 0x0900_0000;

const UART_DR: usize = 0x00;
const UART_FR: usize = 0x18;
const FR_TXFF: u32 = 1 << 5;

#[inline(always)]
fn mmio_write(addr: usize, value: u32) {
    unsafe { (addr as *mut u32).write_volatile(value) }
}

#[inline(always)]
fn mmio_read(addr: usize) -> u32 {
    unsafe { (addr as *const u32).read_volatile() }
}

fn uart_putc(c: u8) {
    while mmio_read(UART0_BASE + UART_FR) & FR_TXFF != 0 {}
    mmio_write(UART0_BASE + UART_DR, c as u32);
}

fn uart_puts(s: &str) {
    for b in s.bytes() {
        if b == b'\n' {
            uart_putc(b'\r');
        }
        uart_putc(b);
    }
}

#[no_mangle]
pub extern "C" fn rust_main() -> ! {
    uart_puts("Hello, world from Rust (bare metal)!\n");

    let mut counter: u64 = 0;
    loop {
        uart_puts("counter = ");
        print_num(counter);
        uart_puts("\n");
        counter += 1;

        delay();
    }
}

fn print_num(mut n: u64) {
    let mut buf = [0u8; 20];
    let mut i = 0;

    if n == 0 {
        uart_putc(b'0');
        return;
    }

    while n > 0 {
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
        i += 1;
    }

    while i > 0 {
        i -= 1;
        uart_putc(buf[i]);
    }
}

fn delay() {
    for _ in 0..5_000_000 {
        unsafe { core::arch::asm!("nop") };
    }
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    uart_puts("panic!\n");
    loop {
        unsafe { core::arch::asm!("wfe") };
    }
}
