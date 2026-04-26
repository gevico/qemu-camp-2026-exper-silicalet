use std::{ffi::CStr, pin::Pin, ptr::NonNull};

use bql::prelude::*;
use common::uninit_field_mut;
use hwcore::prelude::*;
use qom::prelude::*;
use system::prelude::*;
use util::prelude::*;

const EN: u32 = 1;
const POL: u32 = 2;
const TICK: u64 = 100_000;
const N: usize = 4;

#[derive(Clone, Copy, Default)]
struct Ch {
    ctrl: u32,
    period: u32,
    duty: u32,
    beg: u64,
    done: bool,
}

#[derive(Default)]
struct Regs {
    ch: [Ch; N],
}

struct PwmTimer {
    t: Timer,
    p: NonNull<G233PwmState>,
    id: usize,
}

unsafe impl Sync for PwmTimer {}

impl PwmTimer {
    fn new(p: *const G233PwmState, id: usize) -> Self {
        Self {
            t: unsafe { Timer::new() },
            p: NonNull::new(p.cast_mut()).unwrap(),
            id,
        }
    }

    fn init(mut s: Pin<&mut Self>) {
        Timer::init_full(s.as_mut(), None, CLOCK_VIRTUAL, Timer::NS, 0, PwmTimer::hit, |x| {
            &mut x.t
        });
    }

    fn hit(&self) {
        let s = unsafe { self.p.as_ref() };
        let mut r = s.regs.borrow_mut();
        r.ch[self.id].done = true;
    }
}

#[repr(C)]
#[derive(qom::Object, hwcore::Device)]
pub struct G233PwmState {
    parent_obj: ParentField<SysBusDevice>,
    iomem: MemoryRegion,
    regs: BqlRefCell<Regs>,
    tm: [PwmTimer; N],
}

qom_isa!(G233PwmState : SysBusDevice, DeviceState, Object);

unsafe impl ObjectType for G233PwmState {
    type Class = <SysBusDevice as ObjectType>::Class;
    const TYPE_NAME: &'static CStr = c"g233-pwm";
}

impl ObjectImpl for G233PwmState {
    type ParentType = SysBusDevice;

    const INSTANCE_INIT: Option<unsafe fn(ParentInit<Self>)> = Some(Self::init);
    const INSTANCE_POST_INIT: Option<fn(&Self)> = Some(Self::post_init);
    const CLASS_INIT: fn(&mut Self::Class) = Self::Class::class_init::<Self>;
}

impl DeviceImpl for G233PwmState {}
impl ResettablePhasesImpl for G233PwmState {
    const HOLD: Option<fn(&Self, ResetType)> = Some(Self::reset_hold);
}
impl SysBusDeviceImpl for G233PwmState {}

impl G233PwmState {
    unsafe fn init(mut this: ParentInit<Self>) {
        static OPS: MemoryRegionOps<G233PwmState> = MemoryRegionOpsBuilder::<G233PwmState>::new()
            .read(&G233PwmState::read)
            .write(&G233PwmState::write)
            .little_endian()
            .impl_sizes(4, 4)
            .build();

        MemoryRegion::init_io(&mut uninit_field_mut!(*this, iomem), &OPS, "g233-pwm", 0x1000);
        uninit_field_mut!(*this, regs).write(Default::default());
        let p = this.as_ptr();
        let mut a = uninit_field_mut!(*this, tm);
        let tm = a.write([
            PwmTimer::new(p, 0),
            PwmTimer::new(p, 1),
            PwmTimer::new(p, 2),
            PwmTimer::new(p, 3),
        ]);
        for x in tm.iter_mut() {
            PwmTimer::init(unsafe { Pin::new_unchecked(x) });
        }
    }

    fn post_init(&self) {
        self.init_mmio(&self.iomem);
    }

    fn reset_hold(&self, _ty: ResetType) {
        for x in self.tm.iter() {
            x.t.delete();
        }
        *self.regs.borrow_mut() = Regs::default();
    }

    fn arm(&self, r: &mut Regs, id: usize) {
        let c = &mut r.ch[id];
        self.tm[id].t.delete();
        if c.ctrl & EN == 0 {
            return;
        }
        c.beg = CLOCK_VIRTUAL.get_ns();
        let dt = (c.period as u64).saturating_add(1) * TICK;
        self.tm[id].t.modify_ns(c.beg.saturating_add(dt));
    }

    fn glb(r: &Regs) -> u32 {
        let mut x = 0;
        for i in 0..N {
            if r.ch[i].ctrl & EN != 0 {
                x |= 1 << i;
            }
            if r.ch[i].done {
                x |= 1 << (i + 4);
            }
        }
        x
    }

    fn cnt(c: &Ch) -> u32 {
        if c.ctrl & EN == 0 {
            return 0;
        }
        let p = c.period.saturating_add(1) as u64;
        if p == 0 {
            return 0;
        }
        let now = CLOCK_VIRTUAL.get_ns();
        let dt = now.saturating_sub(c.beg) / TICK;
        (dt % p) as u32
    }

    fn read(&self, addr: hwaddr, _size: u32) -> u64 {
        let r = self.regs.borrow();
        if addr == 0x00 {
            return Self::glb(&r).into();
        }
        if !(0x10..0x50).contains(&addr) {
            return 0;
        }
        let id = ((addr - 0x10) / 0x10) as usize;
        let off = (addr - 0x10) % 0x10;
        let c = &r.ch[id];
        match off {
            0x00 => c.ctrl,
            0x04 => c.period,
            0x08 => c.duty,
            0x0c => Self::cnt(c),
            _ => 0,
        }
        .into()
    }

    fn write(&self, addr: hwaddr, val: u64, _size: u32) {
        let mut r = self.regs.borrow_mut();
        let x = val as u32;
        if addr == 0x00 {
            for i in 0..N {
                if x & (1 << (i + 4)) != 0 {
                    r.ch[i].done = false;
                }
            }
            return;
        }
        if !(0x10..0x50).contains(&addr) {
            return;
        }
        let id = ((addr - 0x10) / 0x10) as usize;
        let off = (addr - 0x10) % 0x10;
        let c = &mut r.ch[id];
        match off {
            0x00 => c.ctrl = x & (EN | POL),
            0x04 => c.period = x,
            0x08 => c.duty = x,
            _ => return,
        }
        self.arm(&mut r, id);
    }
}

#[no_mangle]
pub unsafe extern "C" fn g233_pwm_create(addr: u64) -> *mut DeviceState {
    let dev = G233PwmState::new();
    dev.sysbus_realize().unwrap_fatal();
    dev.mmio_map(0, addr);
    dev.as_mut_ptr()
}
