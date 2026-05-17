//! SG2002 复位控制器（Reset Controller）驱动。
//!
//! 复位控制器基地址：`0x03003000`
//!
//! | 寄存器名称       | 偏移地址 | 描述                           |
//! |------------------|----------|--------------------------------|
//! | SOFT_RSTN_0      | 0x000    | 软复位控制寄存器 0             |
//! | SOFT_RSTN_1      | 0x004    | 软复位控制寄存器 1             |
//! | SOFT_RSTN_2      | 0x008    | 软复位控制寄存器 2             |
//! | SOFT_RSTN_3      | 0x00c    | 软复位控制寄存器 3             |
//! | SOFT_CPUAC_RSTN  | 0x020    | CPU 自动清除软复位控制寄存器   |
//! | SOFT_CPU_RSTN    | 0x024    | CPU 软复位控制寄存器           |
//!
//! 复位配置为低电平有效，写 1 解除复位。

#![allow(dead_code)]

use tock_registers::{
    interfaces::{Readable, Writeable},
    register_bitfields, register_structs,
    registers::ReadWrite,
};

pub const RSTC_BASE: usize = 0x03003000;

register_bitfields! [
    u32,

    pub SOFT_RSTN_0 [
        REG_SOFT_RESET_X_DDR    OFFSET(2)  NUMBITS(1) [],
        REG_SOFT_RESET_X_H264C  OFFSET(3)  NUMBITS(1) [],
        REG_SOFT_RESET_X_JPEG   OFFSET(4)  NUMBITS(1) [],
        REG_SOFT_RESET_X_H265C  OFFSET(5)  NUMBITS(1) [],
        REG_SOFT_RESET_X_VIPSYS OFFSET(6)  NUMBITS(1) [],
        REG_SOFT_RESET_X_TDMA   OFFSET(7)  NUMBITS(1) [],
        REG_SOFT_RESET_X_TPU    OFFSET(8)  NUMBITS(1) [],
        REG_SOFT_RESET_X_TPUSYS OFFSET(9)  NUMBITS(1) [],
        REG_SOFT_RESET_X_USB    OFFSET(11) NUMBITS(1) [],
        REG_SOFT_RESET_X_ETH0   OFFSET(12) NUMBITS(1) [],
        REG_SOFT_RESET_X_ETH1   OFFSET(13) NUMBITS(1) [],
        REG_SOFT_RESET_X_NAND   OFFSET(14) NUMBITS(1) [],
        REG_SOFT_RESET_X_EMMC   OFFSET(15) NUMBITS(1) [],
        REG_SOFT_RESET_X_SD0    OFFSET(16) NUMBITS(1) [],
    ],

    pub SOFT_RSTN_1 [
        ALL OFFSET(0) NUMBITS(32) [],
    ],

    pub SOFT_RSTN_2 [
        ALL OFFSET(0) NUMBITS(32) [],
    ],

    pub SOFT_RSTN_3 [
        ALL OFFSET(0) NUMBITS(32) [],
    ],

    pub SOFT_CPUAC_RSTN [
        REG_AUTO_CLEAR_RESET_X_CPUCORE0 OFFSET(0) NUMBITS(1) [],
        REG_AUTO_CLEAR_RESET_X_CPUCORE1 OFFSET(1) NUMBITS(1) [],
        REG_AUTO_CLEAR_RESET_X_CPUCORE2 OFFSET(2) NUMBITS(1) [],
        REG_AUTO_CLEAR_RESET_X_CPUCORE3 OFFSET(3) NUMBITS(1) [],
        REG_AUTO_CLEAR_RESET_X_CPUSYS0  OFFSET(4) NUMBITS(1) [],
        REG_AUTO_CLEAR_RESET_X_CPUSYS1  OFFSET(5) NUMBITS(1) [],
        REG_AUTO_CLEAR_RESET_X_CPUSYS2  OFFSET(6) NUMBITS(1) [],
    ],

    pub SOFT_CPU_RSTN [
        REG_SOFT_RESET_X_CPUCORE0 OFFSET(0) NUMBITS(1) [],
        REG_SOFT_RESET_X_CPUCORE1 OFFSET(1) NUMBITS(1) [],
        REG_SOFT_RESET_X_CPUCORE2 OFFSET(2) NUMBITS(1) [],
        REG_SOFT_RESET_X_CPUCORE3 OFFSET(3) NUMBITS(1) [],
        REG_SOFT_RESET_X_CPUSYS0  OFFSET(4) NUMBITS(1) [],
        REG_SOFT_RESET_X_CPUSYS1  OFFSET(5) NUMBITS(1) [],
        REG_SOFT_RESET_X_CPUSYS2  OFFSET(6) NUMBITS(1) [],
    ],
];

register_structs! {
    /// 复位控制器寄存器组
    pub RstcRegisters {
        (0x000 => pub soft_rstn_0:     ReadWrite<u32, SOFT_RSTN_0::Register>),
        (0x004 => pub soft_rstn_1:     ReadWrite<u32, SOFT_RSTN_1::Register>),
        (0x008 => pub soft_rstn_2:     ReadWrite<u32, SOFT_RSTN_2::Register>),
        (0x00c => pub soft_rstn_3:     ReadWrite<u32, SOFT_RSTN_3::Register>),
        (0x010 => _reserved0),
        (0x020 => pub soft_cpuac_rstn: ReadWrite<u32, SOFT_CPUAC_RSTN::Register>),
        (0x024 => pub soft_cpu_rstn:   ReadWrite<u32, SOFT_CPU_RSTN::Register>),
        (0x028 => @END),
    }
}

/// 复位控制器驱动结构体
pub struct Rstc {
    regs: &'static RstcRegisters,
}

impl Rstc {
    /// 创建新的复位控制器驱动实例
    ///
    /// # Safety
    /// 调用者必须确保寄存器地址有效且可访问，且不会出现并发访问。
    pub unsafe fn new() -> Self {
        unsafe {
            Self {
                regs: &*(RSTC_BASE as *const RstcRegisters),
            }
        }
    }

    /// 从指定基地址创建复位控制器驱动实例
    ///
    /// # Safety
    /// 调用者必须确保基地址有效且可访问。
    pub unsafe fn from_base_address(base: usize) -> Self {
        unsafe {
            Self {
                regs: &*(base as *const RstcRegisters),
            }
        }
    }

    pub fn assert_cpu_core_reset(&self, core: u8) {
        let mask = 1u32 << core;
        let val = self.regs.soft_cpu_rstn.get();
        self.regs.soft_cpu_rstn.set(val & !mask);
    }

    pub fn release_cpu_core_reset(&self, core: u8) {
        let mask = 1u32 << core;
        let val = self.regs.soft_cpu_rstn.get();
        self.regs.soft_cpu_rstn.set(val | mask);
    }

    pub fn assert_cpu_sys_reset(&self, sys: u8) {
        let mask = 1u32 << (4 + sys);
        let val = self.regs.soft_cpu_rstn.get();
        self.regs.soft_cpu_rstn.set(val & !mask);
    }

    pub fn release_cpu_sys_reset(&self, sys: u8) {
        let mask = 1u32 << (4 + sys);
        let val = self.regs.soft_cpu_rstn.get();
        self.regs.soft_cpu_rstn.set(val | mask);
    }

    pub fn trigger_cpu_core_auto_reset(&self, core: u8) {
        let mask = 1u32 << core;
        let val = self.regs.soft_cpuac_rstn.get();
        self.regs.soft_cpuac_rstn.set(val & !mask);
    }

    pub fn trigger_cpu_sys_auto_reset(&self, sys: u8) {
        let mask = 1u32 << (4 + sys);
        let val = self.regs.soft_cpuac_rstn.get();
        self.regs.soft_cpuac_rstn.set(val & !mask);
    }

    pub fn read_soft_cpu_rstn(&self) -> u32 {
        self.regs.soft_cpu_rstn.get()
    }

    pub fn write_soft_cpu_rstn(&self, value: u32) {
        self.regs.soft_cpu_rstn.set(value);
    }

    pub fn read_soft_cpuac_rstn(&self) -> u32 {
        self.regs.soft_cpuac_rstn.get()
    }

    pub fn write_soft_cpuac_rstn(&self, value: u32) {
        self.regs.soft_cpuac_rstn.set(value);
    }

    /// 获取寄存器组引用
    pub fn regs(&self) -> &'static RstcRegisters {
        self.regs
    }
}
