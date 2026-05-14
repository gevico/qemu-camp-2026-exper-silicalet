use std::ffi::CStr;

use bql::prelude::*;
use common::uninit_field_mut;
use hwcore::IRQState;
use hwcore::prelude::*;
use qom::prelude::*;
use system::prelude::*;
use util::prelude::*;

#[derive(Default)]
struct Regs {
    dir: u32,
    out: u32,
    input: u32,
    ie: u32,
    is: u32,
    trig: u32,
    pol: u32,
}

#[repr(C)]
#[derive(qom::Object, hwcore::Device)]
pub struct G233GpioState {
    parent_obj: ParentField<SysBusDevice>,
    iomem: MemoryRegion,
    irq: InterruptSource,
    regs: BqlRefCell<Regs>,
}

qom_isa!(G233GpioState : SysBusDevice, DeviceState, Object);

unsafe impl ObjectType for G233GpioState {
    type Class = <SysBusDevice as ObjectType>::Class;
    const TYPE_NAME: &'static CStr = c"g233-gpio";
}

impl ObjectImpl for G233GpioState {
    type ParentType = SysBusDevice;

    const INSTANCE_INIT: Option<unsafe fn(ParentInit<Self>)> = Some(Self::init);
    const INSTANCE_POST_INIT: Option<fn(&Self)> = Some(Self::post_init);
    const CLASS_INIT: fn(&mut Self::Class) = Self::Class::class_init::<Self>;
}

impl DeviceImpl for G233GpioState {}
impl ResettablePhasesImpl for G233GpioState {
    const HOLD: Option<fn(&Self, ResetType)> = Some(Self::reset_hold);
}
impl SysBusDeviceImpl for G233GpioState {}

impl G233GpioState {
    unsafe fn init(mut this: ParentInit<Self>) {
        static OPS: MemoryRegionOps<G233GpioState> = MemoryRegionOpsBuilder::<G233GpioState>::new()
            .read(&G233GpioState::read)
            .write(&G233GpioState::write)
            .little_endian()
            .impl_sizes(4, 4)
            .build();

        MemoryRegion::init_io(&mut uninit_field_mut!(*this, iomem), &OPS, "g233-gpio", 0x1000);
        uninit_field_mut!(*this, irq).write(Default::default());
        uninit_field_mut!(*this, regs).write(Default::default());
    }

    fn post_init(&self) {
        self.init_mmio(&self.iomem);
        self.init_irq(&self.irq);
    }

    fn reset_hold(&self, _ty: ResetType) {
        *self.regs.borrow_mut() = Regs::default();
        self.irq.lower();
    }

    fn inp(r: &Regs) -> u32 {
        r.out & r.dir
    }

    fn sync(&self, r: &mut Regs, old: u32, clear: u32) {
        let now = Self::inp(r);
        r.input = now;
        let up = !old & now;
        let dn = old & !now;
        let ed = !r.trig;
        let lv = r.trig;
        let ev = ed & r.ie & ((r.pol & up) | (!r.pol & dn));
        let act = lv & r.ie & ((r.pol & now) | (!r.pol & !now));
        r.is &= !clear;
        r.is = (r.is & !lv) | act;
        r.is |= ev;
        if r.is != 0 {
            self.irq.raise();
        } else {
            self.irq.lower();
        }
    }

    fn read(&self, addr: hwaddr, _size: u32) -> u64 {
        let r = self.regs.borrow();
        match addr {
            0x00 => r.dir,
            0x04 => r.out,
            0x08 => r.input,
            0x0c => r.ie,
            0x10 => r.is,
            0x14 => r.trig,
            0x18 => r.pol,
            _ => 0,
        }
        .into()
    }

    fn write(&self, addr: hwaddr, val: u64, _size: u32) {
        let mut r = self.regs.borrow_mut();
        let old = r.input;
        let x = val as u32;
        match addr {
            0x00 => r.dir = x,
            0x04 => r.out = x,
            0x0c => r.ie = x,
            0x10 => {
                self.sync(&mut r, old, x);
                return;
            }
            0x14 => r.trig = x,
            0x18 => r.pol = x,
            _ => return,
        }
        self.sync(&mut r, old, 0);
    }
}

#[no_mangle]
pub unsafe extern "C" fn g233_gpio_create(addr: u64, irq: *mut IRQState) -> *mut DeviceState {
    let irq = unsafe { Owned::<IRQState>::from(&*irq) };
    let dev = G233GpioState::new();
    dev.sysbus_realize().unwrap_fatal();
    dev.mmio_map(0, addr);
    dev.connect_irq(0, &irq);
    dev.as_mut_ptr()
}

const RI2C_CTRL_EN: u32 = 1 << 0;
const RI2C_CTRL_START: u32 = 1 << 1;
const RI2C_CTRL_STOP: u32 = 1 << 2;
const RI2C_CTRL_RW: u32 = 1 << 3;

const RI2C_ST_BUSY: u32 = 1 << 0;
const RI2C_ST_ACK: u32 = 1 << 1;
const RI2C_ST_DONE: u32 = 1 << 2;

const RI2C_EEPROM_ADDR: u8 = 0x50;
const RI2C_PAGE_MASK: u8 = !0x07;

#[derive(Clone, Copy, Default, PartialEq, Eq)]
enum RustI2cPhase {
    #[default]
    Idle,
    Write,
    Read,
}

struct RustI2cRegs {
    ctrl: u32,
    status: u32,
    addr: u32,
    data: u32,
    prescale: u32,
    eeprom: [u8; 256],
    pointer: u8,
    page_base: u8,
    page_off: u8,
    expect_addr: bool,
    phase: RustI2cPhase,
}

impl Default for RustI2cRegs {
    fn default() -> Self {
        Self {
            ctrl: 0,
            status: 0,
            addr: 0,
            data: 0,
            prescale: 0,
            eeprom: [0xff; 256],
            pointer: 0,
            page_base: 0,
            page_off: 0,
            expect_addr: false,
            phase: RustI2cPhase::Idle,
        }
    }
}

#[repr(C)]
#[derive(qom::Object, hwcore::Device)]
pub struct G233RustI2cState {
    parent_obj: ParentField<SysBusDevice>,
    iomem: MemoryRegion,
    irq: InterruptSource,
    regs: BqlRefCell<RustI2cRegs>,
}

qom_isa!(G233RustI2cState : SysBusDevice, DeviceState, Object);

unsafe impl ObjectType for G233RustI2cState {
    type Class = <SysBusDevice as ObjectType>::Class;
    const TYPE_NAME: &'static CStr = c"g233-rust-i2c";
}

impl ObjectImpl for G233RustI2cState {
    type ParentType = SysBusDevice;

    const INSTANCE_INIT: Option<unsafe fn(ParentInit<Self>)> = Some(Self::init);
    const INSTANCE_POST_INIT: Option<fn(&Self)> = Some(Self::post_init);
    const CLASS_INIT: fn(&mut Self::Class) = Self::Class::class_init::<Self>;
}

impl DeviceImpl for G233RustI2cState {}
impl ResettablePhasesImpl for G233RustI2cState {
    const HOLD: Option<fn(&Self, ResetType)> = Some(Self::reset_hold);
}
impl SysBusDeviceImpl for G233RustI2cState {}

impl G233RustI2cState {
    unsafe fn init(mut this: ParentInit<Self>) {
        static OPS: MemoryRegionOps<G233RustI2cState> =
            MemoryRegionOpsBuilder::<G233RustI2cState>::new()
                .read(&G233RustI2cState::read)
                .write(&G233RustI2cState::write)
                .little_endian()
                .impl_sizes(4, 4)
                .build();

        MemoryRegion::init_io(
            &mut uninit_field_mut!(*this, iomem),
            &OPS,
            "g233-rust-i2c",
            0x1000,
        );
        uninit_field_mut!(*this, irq).write(Default::default());
        uninit_field_mut!(*this, regs).write(Default::default());
    }

    fn post_init(&self) {
        self.init_mmio(&self.iomem);
        self.init_irq(&self.irq);
    }

    fn reset_hold(&self, _ty: ResetType) {
        *self.regs.borrow_mut() = RustI2cRegs::default();
        self.irq.lower();
    }

    fn finish(r: &mut RustI2cRegs, ack: bool) {
        let mut status = RI2C_ST_DONE;
        if ack {
            status |= RI2C_ST_ACK;
        }
        if r.phase != RustI2cPhase::Idle {
            status |= RI2C_ST_BUSY;
        }
        r.status = status;
    }

    fn run(r: &mut RustI2cRegs) {
        let enabled = r.ctrl & RI2C_CTRL_EN != 0;
        let rw = r.ctrl & RI2C_CTRL_RW != 0;
        let hit = (r.addr & 0x7f) as u8 == RI2C_EEPROM_ADDR;

        if !enabled {
            r.phase = RustI2cPhase::Idle;
            r.status = 0;
            return;
        }

        if r.ctrl & RI2C_CTRL_STOP != 0 {
            r.phase = RustI2cPhase::Idle;
            r.expect_addr = false;
            Self::finish(r, hit);
            return;
        }

        if r.ctrl & RI2C_CTRL_START != 0 {
            if hit {
                r.phase = if rw {
                    RustI2cPhase::Read
                } else {
                    r.expect_addr = true;
                    RustI2cPhase::Write
                };
                Self::finish(r, true);
            } else {
                r.phase = RustI2cPhase::Idle;
                r.expect_addr = false;
                Self::finish(r, false);
            }
            return;
        }

        if !hit {
            Self::finish(r, false);
            return;
        }

        match r.phase {
            RustI2cPhase::Write if !rw => {
                if r.expect_addr {
                    r.pointer = r.data as u8;
                    r.page_base = r.pointer & RI2C_PAGE_MASK;
                    r.page_off = r.pointer & !RI2C_PAGE_MASK;
                    r.expect_addr = false;
                } else {
                    let idx = (r.page_base | r.page_off) as usize;
                    r.eeprom[idx] = r.data as u8;
                    r.page_off = (r.page_off + 1) & 0x07;
                    r.pointer = r.page_base | r.page_off;
                }
                Self::finish(r, true);
            }
            RustI2cPhase::Read if rw => {
                r.data = r.eeprom[r.pointer as usize].into();
                r.pointer = r.pointer.wrapping_add(1);
                Self::finish(r, true);
            }
            _ => Self::finish(r, false),
        }
    }

    fn read(&self, addr: hwaddr, _size: u32) -> u64 {
        let r = self.regs.borrow();
        match addr {
            0x00 => r.ctrl,
            0x04 => r.status,
            0x08 => r.addr,
            0x0c => r.data,
            0x10 => r.prescale,
            _ => 0,
        }
        .into()
    }

    fn write(&self, addr: hwaddr, val: u64, _size: u32) {
        let mut r = self.regs.borrow_mut();
        let x = val as u32;

        match addr {
            0x00 => {
                r.ctrl = x & (RI2C_CTRL_EN | RI2C_CTRL_START | RI2C_CTRL_STOP | RI2C_CTRL_RW);
                Self::run(&mut r);
            }
            0x08 => r.addr = x & 0x7f,
            0x0c => r.data = x & 0xff,
            0x10 => r.prescale = x,
            _ => {}
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn g233_rust_i2c_create(addr: u64, irq: *mut IRQState) -> *mut DeviceState {
    let irq = unsafe { Owned::<IRQState>::from(&*irq) };
    let dev = G233RustI2cState::new();
    dev.sysbus_realize().unwrap_fatal();
    dev.mmio_map(0, addr);
    dev.connect_irq(0, &irq);
    dev.as_mut_ptr()
}
