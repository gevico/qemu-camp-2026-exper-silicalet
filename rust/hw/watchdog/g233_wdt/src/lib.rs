use std::{ffi::CStr, pin::Pin, ptr::NonNull};

use bql::prelude::*;
use common::uninit_field_mut;
use hwcore::IRQState;
use hwcore::prelude::*;
use qom::prelude::*;
use system::prelude::*;
use util::prelude::*;

const EN: u32 = 1;
const INTEN: u32 = 2;
const FEED: u32 = 0x5a5a5a5a;
const LOCK: u32 = 0x1acce551;
const TIMEOUT: u32 = 1;
const TICK: u64 = 100_000;

#[derive(Default)]
struct Regs {
    ctrl: u32,
    load: u32,
    sr: u32,
    lock: bool,
    beg: u64,
}

struct WdtTimer {
    t: Timer,
    p: NonNull<G233WdtState>,
}

unsafe impl Sync for WdtTimer {}

impl WdtTimer {
    fn new(p: *const G233WdtState) -> Self {
        Self {
            t: unsafe { Timer::new() },
            p: NonNull::new(p.cast_mut()).unwrap(),
        }
    }

    fn init(mut s: Pin<&mut Self>) {
        Timer::init_full(s.as_mut(), None, CLOCK_VIRTUAL, Timer::NS, 0, WdtTimer::hit, |x| {
            &mut x.t
        });
    }

    fn hit(&self) {
        let s = unsafe { self.p.as_ref() };
        let mut r = s.regs.borrow_mut();
        r.sr |= TIMEOUT;
        r.ctrl &= !EN;
        s.upd(&r);
    }
}

#[repr(C)]
#[derive(qom::Object, hwcore::Device)]
pub struct G233WdtState {
    parent_obj: ParentField<SysBusDevice>,
    iomem: MemoryRegion,
    irq: InterruptSource,
    regs: BqlRefCell<Regs>,
    tim: WdtTimer,
}

qom_isa!(G233WdtState : SysBusDevice, DeviceState, Object);

unsafe impl ObjectType for G233WdtState {
    type Class = <SysBusDevice as ObjectType>::Class;
    const TYPE_NAME: &'static CStr = c"g233-wdt";
}

impl ObjectImpl for G233WdtState {
    type ParentType = SysBusDevice;

    const INSTANCE_INIT: Option<unsafe fn(ParentInit<Self>)> = Some(Self::init);
    const INSTANCE_POST_INIT: Option<fn(&Self)> = Some(Self::post_init);
    const CLASS_INIT: fn(&mut Self::Class) = Self::Class::class_init::<Self>;
}

impl DeviceImpl for G233WdtState {}
impl ResettablePhasesImpl for G233WdtState {
    const HOLD: Option<fn(&Self, ResetType)> = Some(Self::reset_hold);
}
impl SysBusDeviceImpl for G233WdtState {}

impl G233WdtState {
    unsafe fn init(mut this: ParentInit<Self>) {
        static OPS: MemoryRegionOps<G233WdtState> = MemoryRegionOpsBuilder::<G233WdtState>::new()
            .read(&G233WdtState::read)
            .write(&G233WdtState::write)
            .little_endian()
            .impl_sizes(4, 4)
            .build();

        MemoryRegion::init_io(&mut uninit_field_mut!(*this, iomem), &OPS, "g233-wdt", 0x1000);
        uninit_field_mut!(*this, irq).write(Default::default());
        uninit_field_mut!(*this, regs).write(Default::default());
        let p = this.as_ptr();
        let mut a = uninit_field_mut!(*this, tim);
        let tim = a.write(WdtTimer::new(p));
        WdtTimer::init(unsafe { Pin::new_unchecked(tim) });
    }

    fn post_init(&self) {
        self.init_mmio(&self.iomem);
        self.init_irq(&self.irq);
    }

    fn reset_hold(&self, _ty: ResetType) {
        self.tim.t.delete();
        *self.regs.borrow_mut() = Regs::default();
        self.irq.lower();
    }

    fn upd(&self, r: &Regs) {
        if r.ctrl & INTEN != 0 && r.sr & TIMEOUT != 0 {
            self.irq.raise();
        } else {
            self.irq.lower();
        }
    }

    fn val(r: &Regs) -> u32 {
        if r.ctrl & EN == 0 {
            return r.load;
        }
        let now = CLOCK_VIRTUAL.get_ns();
        let dt = now.saturating_sub(r.beg) / TICK;
        r.load.saturating_sub(dt as u32)
    }

    fn arm(&self, r: &mut Regs) {
        if r.ctrl & EN == 0 {
            self.tim.t.delete();
            return;
        }
        r.beg = CLOCK_VIRTUAL.get_ns();
        let dt = (r.load as u64).saturating_add(1) * TICK;
        self.tim.t.modify_ns(r.beg.saturating_add(dt));
    }

    fn read(&self, addr: hwaddr, _size: u32) -> u64 {
        let r = self.regs.borrow();
        match addr {
            0x00 => r.ctrl,
            0x04 => r.load,
            0x08 => Self::val(&r),
            0x0c => 0,
            0x10 => r.sr,
            _ => 0,
        }
        .into()
    }

    fn write(&self, addr: hwaddr, val: u64, _size: u32) {
        let mut r = self.regs.borrow_mut();
        let x = val as u32;
        match addr {
            0x00 => {
                if !r.lock {
                    r.ctrl = x & (EN | INTEN);
                    self.arm(&mut r);
                }
            }
            0x04 => {
                if !r.lock {
                    r.load = x;
                    if r.ctrl & EN != 0 {
                        self.arm(&mut r);
                    }
                }
            }
            0x0c => {
                if x == FEED {
                    self.arm(&mut r);
                    r.sr &= !TIMEOUT;
                } else if x == LOCK {
                    r.lock = true;
                }
            }
            0x10 => r.sr &= !x,
            _ => return,
        }
        self.upd(&r);
    }
}

#[no_mangle]
pub unsafe extern "C" fn g233_wdt_create(addr: u64, irq: *mut IRQState) -> *mut DeviceState {
    let irq = unsafe { Owned::<IRQState>::from(&*irq) };
    let dev = G233WdtState::new();
    dev.sysbus_realize().unwrap_fatal();
    dev.mmio_map(0, addr);
    dev.connect_irq(0, &irq);
    dev.as_mut_ptr()
}
