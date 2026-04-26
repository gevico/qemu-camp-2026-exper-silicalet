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
