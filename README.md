ch432 SPI Serial Driver
===========================
This driver can only work with SPI to Dual UARTs chip ch432.

Integrated into your system method1
---------------------------------------
If you are using dts device tree to config spi and driver, you can read this method, otherwise
please refer to method2.

1. Please copy the driver file to the package directory which be used to add additional drivers.

2. Please add the relevant Makefile and Kconfig like other drivers, generally you can copy one
from other driver then modify it.

3. Run the make menuconfig and select the ch432 serial support at "modules" item.

4. Define the spi structure on your dts file similar the follow: 
	spidev@1 {
		#address-cells = <1>;
		#size-cells = <1>;
		compatible = "ch43x_spi";
		reg = <1 0>;
		spi-max-frequency = <5000000>;
		interrupt-parent = <&gpio0>;
		interrupts = <0 2>;
	}
	Notice that the irq request method cannot be supported in this way in some platforms.
	You should modify it in ch432.c in method ch43x_spi_probe.

Integrated into your system method2
---------------------------------------
1. Please copy the driver file to the kernel directory:$kernel_src\drivers\tty\serial

2. Please add the followed txt into the kernel file:$kernel_src\drivers\tty\serial\Konfig
config SERIAL_CH432
	tristate "SERIAL_CH432 serial support"
	depends on SPI
	select SERIAL_CORE
	help
	  This selects support for ch432 serial ports.
	
3. Add the follow define into the $kernel_src\drivers\tty\serial\Makefile for compile the driver.
obj-$(CONFIG_SERIAL_CH43X) += ch432.o

4. Run the make menuconfig and select the ch432 serial support at the driver/tty/serial and save the config.

5. Define the spi0_board_info object on your board file similar the follow:
static struct spi_board_info spi0_board_info[] __initdata = {
	{
		.modalias = "ch43x_spi",
		.platform_data = NULL,
		.max_speed_hz = 100 * 1000,
		.bus_num = 0,
		.chip_select = 0,
		.mode = SPI_MODE_0,
		.controller_data = &spi0_csi[0],
		.irq		= IRQ_EINT(25),
	}
};


* if you need change the external xtal freq, you can modify it at about line:57, 
  \#define CRYSTAL_FREQ 22118400

**Note**
  Any question, you can send feedback to mail: tech@wch.cn