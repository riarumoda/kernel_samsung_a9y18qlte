#!/bin/bash

# Riaru Kernel Simple Build System
# Here is the dependencies to build the kernel
# Ubuntu/Ubuntu Based OS: apt update && apt upgrade && apt install glibc-source libghc-libyaml-dev libyaml-dev binutils-aarch64-linux-gnu binutils-arm-linux-gnueabi binutils device-tree-compiler libtfm-dev libelf-dev cpio kmod nano bc bison ca-certificates curl flex gcc git libc6-dev libssl-dev openssl python-is-python3 ssh wget zip zstd sudo make clang gcc-arm-linux-gnueabi software-properties-common build-essential libarchive-tools gcc-aarch64-linux-gnu python2
# Arch/Arch Based OS: sudo pacman -S aarch64-linux-gnu-glibc glibc libyaml aarch64-linux-gnu-binutils arm-none-eabi-binutils binutils dtc fmt libelf cpio kmod bc bison ca-certificates curl flex glibc openssl openssh wget zip zstd make clang aarch64-linux-gnu-gcc arm-none-eabi-gcc archivetools base-devel python git

# Logs
echo "Removing previous kernel build logs..."
LOGGER=kernel-logs.txt
REALLOGGER="$(pwd)"/${LOGGER}
rm -rf $REALLOGGER

# Manual clock sync for WSL, look at your /usr/share/zoneinfo/* to see more info
ISTHISWSL=0
CONTINENTS=Asia
LOCATION=Makassar
if [ "$ISTHISWSL" == "1" ]; then
	echo "Asking sudo password for updating the timezone manually..."
	sudo ln -sf /usr/share/zoneinfo/$CONTINENTS/$LOCATION /etc/localtime &>> $REALLOGGER
	sudo hwclock --systohc &>> $REALLOGGER
fi

# LineageOS 19.1 Google GCC 4.9
NOTHAVEGCC=1
LOSGCC_DIR="los-gcc"
REALLOSGCC_DIR="$(pwd)"/${LOSGCC_DIR}
if [ "$NOTHAVEGCC" == "1" ]; then
	echo "Cloning Google GCC 4.9 from LienageOS Repository..."
	git clone https://github.com/LineageOS/android_prebuilts_gcc_linux-x86_aarch64_aarch64-linux-android-4.9 $LOSGCC_DIR &>> $REALLOGGER
fi
echo "Setting up proper permissions to GCC and export it to PATH..."
sudo chmod 755 -R $REALLOSGCC_DIR &>> $REALLOGGER
export PATH="$REALLOSGCC_DIR/bin:$PATH"

# Variables
export KBUILD_BUILD_USER="$USER"
export KBUILD_BUILD_HOST="$HOSTNAME"
export CROSS_COMPILE=aarch64-linux-android-
export ARCH=arm64
export SUBARCH=arm64
export HEADER_ARCH=arm64
DEFCONFIG=a9y18qlte_eur_open_defconfig

# Cleanup
echo "Cleaning up out directory..."
rm -rf out
mkdir out
rm -rf error.log

# Compile
echo "Compiling the kernel..."
make -j16 O=out clean &>> $REALLOGGER
make -j16 O=out $DEFCONFIG &>> $REALLOGGER
make -j16 ARCH=arm64 O=out SUBARCH=arm64 O=out \
        CC=${REALLOSGCC_DIR}/bin/aarch64-linux-android-gcc \
        LD=${REALLOSGCC_DIR}/bin/aarch64-linux-android-ld.bfd \
        AR=${REALLOSGCC_DIR}/bin/aarch64-linux-android-ar \
        AS=${REALLOSGCC_DIR}/bin/aarch64-linux-android-as \
        NM=${REALLOSGCC_DIR}/bin/aarch64-linux-android-nm \
        OBJCOPY=${REALLOSGCC_DIR}/bin/aarch64-linux-android-objcopy \
        OBJDUMP=${REALLOSGCC_DIR}/bin/aarch64-linux-android-objdump \
        STRIP=${REALLOSGCC_DIR}/bin/aarch64-linux-android-strip \
        CROSS_COMPILE=${REALLOSGCC_DIR}/bin/aarch64-linux-android- &>> $REALLOGGER
