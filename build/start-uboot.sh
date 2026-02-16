sudo ../../allwinner-xfel/xfel ddr t113-s3

# Enable UART0 clock
sudo ../../allwinner-xfel/xfel write32 0x02001940 0x80010001

# Set PE2=func6(UART_TX), PE3=func6(UART_RX) in PE_CFG0
sudo ../../allwinner-xfel/xfel read32 0x020000C0
sudo ../../allwinner-xfel/xfel write32 0x020000C0 0xffff66ff

# Init UART
sudo ../../allwinner-xfel/xfel write32 0x0250000C 0x00000080
sudo ../../allwinner-xfel/xfel write32 0x02500000 0x0000000D
sudo ../../allwinner-xfel/xfel write32 0x02500004 0x00000000
sudo ../../allwinner-xfel/xfel write32 0x0250000C 0x00000003
sudo ../../allwinner-xfel/xfel write32 0x02500008 0x00000001

# Test
sudo ../../allwinner-xfel/xfel write32 0x02500000 0x00000021

# Load and run U-Boot
sudo ../../allwinner-xfel/xfel write 0x42e00000 tmp/deploy/images/elegoo-centauri-carbon1/u-boot.bin
sudo ../../allwinner-xfel/xfel exec 0x42e00000
