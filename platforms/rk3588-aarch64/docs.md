setenv tftpblocksize 1468; setenv tftpwindowsize 32
bootdev hunt ethernet; setenv tftpdstp 69; setenv ipaddr 192.168.100.2; setenv serverip 192.168.100.1; tftp 0x400000 kernel_aarch64.bin; tftp 0x10000000 rootfs-aarch64.img; go 0x400000


sudo cp build/kernel_aarch64.bin /srv/tftp
make PLATFORM=rk3588-aarch64 kernel -j8 LOG=warn SMP=8


sudo ip link set enx207bd2d4d4e9 up
sudo ip addr add 192.168.100.1/24 dev enx207bd2d4d4e9
ip addr show enx207bd2d4d4e9


python3 -m miio.cli device --ip  192.168.1.52  --token 9e133ad5c0bbcaa0429e0d8dae4e9aad info

python3 -m miio.cli device --ip 192.168.1.52 --token 9e133ad5c0bbcaa0429e0d8dae4e9aad raw_command set_properties "[{'did': 'MYDID', 'siid': 2, 'piid': 1, 'value':True}]"

python3 -m miio.cli device --ip 192.168.1.52 --token 9e133ad5c0bbcaa0429e0d8dae4e9aad raw_command set_properties "[{'did': 'MYDID', 'siid': 2, 'piid': 1, 'value':False}]"


sudo usermod -aG dialout $USER
sudo tee /etc/udev/rules.d/99-ttyusb.rules > /dev/null << 'EOF'
KERNEL=="ttyUSB*", MODE="0666"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger


minicom -D /dev/ttyUSB0 -b 1500000 


	vcc5v0-usbdcin {
		compatible = "regulator-fixed";
		regulator-name = "vcc5v0_usbdcin";
		regulator-always-on;
		regulator-boot-on;
		regulator-min-microvolt = <0x4c4b40>;
		regulator-max-microvolt = <0x4c4b40>;
		vin-supply = <0x1c0>;
		phandle = <0x1c1>;
	};

	vcc5v0-usb {
		compatible = "regulator-fixed";
		regulator-name = "vcc5v0_usb";
		regulator-always-on;
		regulator-boot-on;
		regulator-min-microvolt = <0x4c4b40>;
		regulator-max-microvolt = <0x4c4b40>;
		vin-supply = <0x1c1>;
		phandle = <0x1cc>;
	};