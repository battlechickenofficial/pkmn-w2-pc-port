//! Static recompiler for the B2W2 Windows port.
//!
//! Reads the modules of a dsd extract (binaries) and dsd config (function symbols), decodes every function with
//! unarm, and writes one C function per guest function. Generated code runs on the runtime in `port/runtime`:
//! guest registers live in `Cpu`, guest memory is reached through `rd*/wr*`, and anything that needs the machine
//! (coprocessor, SWI, PSR writes, indirect calls) goes through an `rc_*` runtime call.
//!
//! Usage: recomp <version> <out_dir> [module ...]     modules: main itcm dtcm autoload_2 autoload_3 ovNNN | all

use std::collections::{BTreeMap, BTreeSet, HashMap, VecDeque};
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

use unarm::*;

// ------------------------------------------------------------------ modules and symbols

struct Module {
    /// C identifier prefix, e.g. "main", "ov036"
    name: String,
    /// Overlay id, or None for modules that are always resident
    overlay: Option<u32>,
    /// For code the game copies before running it: the address it's copied from
    copy_src: Option<u32>,
    base: u32,
    data: Vec<u8>,
    /// start address -> (thumb, end address)
    funcs: BTreeMap<u32, (bool, u32)>,
    /// Labels dsd found other code calling into, which get their own entry
    labels: Vec<(u32, bool)>,
}

impl Module {
    fn end(&self) -> u32 {
        self.base + self.data.len() as u32
    }
    fn contains(&self, addr: u32) -> bool {
        addr >= self.base && addr < self.end()
    }
    fn u16_at(&self, addr: u32) -> Option<u32> {
        let o = addr.checked_sub(self.base)? as usize;
        Some(u16::from_le_bytes(self.data.get(o..o + 2)?.try_into().unwrap()) as u32)
    }
    fn u32_at(&self, addr: u32) -> Option<u32> {
        let o = addr.checked_sub(self.base)? as usize;
        Some(u32::from_le_bytes(self.data.get(o..o + 4)?.try_into().unwrap()))
    }
    /// The function containing `addr`, if any
    fn func_containing(&self, addr: u32) -> Option<(u32, bool, u32)> {
        let (&start, &(thumb, end)) = self.funcs.range(..=addr).next_back()?;
        (addr < end).then_some((start, thumb, end))
    }
}

fn yaml_u32(text: &str, key: &str) -> u32 {
    text.lines()
        .find_map(|l| l.trim().strip_prefix(key).and_then(|v| v.strip_prefix(':')))
        .unwrap_or_else(|| panic!("{key} not found"))
        .trim()
        .parse()
        .unwrap()
}

fn parse_hex(s: &str) -> u32 {
    u32::from_str_radix(s.trim_start_matches("0x"), 16).unwrap()
}

fn load_symbols(path: &Path, m: &mut Module) {
    let text = fs::read_to_string(path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    for line in text.lines() {
        let mut toks = line.split_whitespace();
        let (Some(_name), Some(kind)) = (toks.next(), toks.next()) else { continue };
        let Some(addr) = toks.find_map(|t| t.strip_prefix("addr:")).map(parse_hex) else { continue };
        if let Some(args) = kind.strip_prefix("kind:function(") {
            let args = args.trim_end_matches(')');
            let thumb = args.starts_with("thumb");
            let size = args.split(',').find_map(|a| a.strip_prefix("size=")).map(parse_hex).unwrap_or(0);
            m.funcs.insert(addr, (thumb, addr + size));
        } else if let Some(args) = kind.strip_prefix("kind:label(") {
            m.labels.push((addr, args.starts_with("thumb")));
        }
    }
}

fn load_module(root: &Path, version: &str, which: &str) -> Module {
    let ex = root.join("extract").join(version);
    let cfg = root.join("config").join(version).join("arm9");
    let (bin, yaml, syms, overlay) = if which == "main" {
        (ex.join("arm9/arm9.bin"), Some(ex.join("arm9/arm9.yaml")), cfg.join("symbols.txt"), None)
    } else if let Some(id) = which.strip_prefix("ov") {
        let id: u32 = id.parse().unwrap();
        (ex.join(format!("arm9_overlays/ov{id:03}.bin")), None, cfg.join(format!("overlays/ov{id:03}/symbols.txt")),
         Some(id))
    } else {
        let stem = if which.starts_with("autoload_") { format!("unk_{which}") } else { which.to_string() };
        (ex.join(format!("arm9/{stem}.bin")), Some(ex.join(format!("arm9/{stem}.yaml"))),
         cfg.join(format!("{which}/symbols.txt")), None)
    };
    let base = match (&yaml, overlay) {
        (Some(y), _) => yaml_u32(&fs::read_to_string(y).unwrap(), "base_address"),
        (None, Some(id)) => overlay_base(&ex, id),
        _ => unreachable!(),
    };
    let data = fs::read(&bin).unwrap_or_else(|e| panic!("{}: {e}", bin.display()));
    let mut m = Module { name: which.to_string(), overlay, copy_src: None, base, data, funcs: BTreeMap::new(),
                         labels: vec![] };
    load_symbols(&syms, &mut m);
    m
}

fn overlay_base(ex: &Path, id: u32) -> u32 {
    let text = fs::read_to_string(ex.join("arm9_overlays/overlays.yaml")).unwrap();
    for block in text.split("\n  - ").skip(1) {
        if yaml_u32(block, "id") == id {
            return yaml_u32(block, "base_address");
        }
    }
    panic!("overlay {id} not in overlays.yaml")
}

fn all_overlays(root: &Path, version: &str) -> Vec<String> {
    let text = fs::read_to_string(root.join("extract").join(version).join("arm9_overlays/overlays.yaml")).unwrap();
    text.split("\n  - ").skip(1).map(|b| format!("ov{:03}", yaml_u32(b, "id"))).collect()
}

/// Modules for the code copies listed in port/code_copies.txt, cut out of the modules already loaded
fn code_copies(root: &Path, version: &str, loaded: &[Module]) -> Vec<Module> {
    let Ok(text) = fs::read_to_string(root.join("port/code_copies.txt")) else { return vec![] };
    let mut out = vec![];
    for line in text.lines().map(|l| l.split('#').next().unwrap().trim()).filter(|l| !l.is_empty()) {
        let f: Vec<&str> = line.split_whitespace().collect();
        if f[0] != version {
            continue;
        }
        let (src, run, size, thumb) = (parse_hex(f[1]), parse_hex(f[2]), parse_hex(f[3]), f[4] == "thumb");
        let Some(from) = loaded.iter().find(|m| m.copy_src.is_none() && m.contains(src) && m.contains(src + size - 1))
        else {
            panic!("code copy source {src:08x} is in no loaded module");
        };
        let o = (src - from.base) as usize;
        let mut funcs = BTreeMap::new();
        funcs.insert(run, (thumb, run + size));
        out.push(Module { name: format!("copy_{src:08x}"), overlay: None, copy_src: Some(src), base: run,
                          data: from.data[o..o + size as usize].to_vec(), funcs, labels: vec![] });
    }
    out
}

const STATIC_MODULES: [&str; 5] = ["main", "itcm", "dtcm", "autoload_2", "autoload_3"];

// ------------------------------------------------------------------ function analysis

#[derive(Clone)]
struct Decoded {
    ins: Ins,
    size: u32,
}

struct FuncCode {
    module: usize,
    start: u32,
    end: u32,
    thumb: bool,
    insns: BTreeMap<u32, Decoded>,
    /// Computed jumps inside the function: instruction address -> possible targets
    tables: HashMap<u32, Vec<u32>>,
    /// Addresses that are targets of in-function branches, which need a label
    targets: BTreeSet<u32>,
}

fn options() -> Options {
    Options { version: Version::V5Te, ual: false, ..Default::default() }
}

fn decode(m: &Module, addr: u32, thumb: bool) -> Option<Decoded> {
    if thumb {
        let lo = m.u16_at(addr)?;
        let hi = m.u16_at(addr + 2).unwrap_or(0);
        let (ins, size) = parse_thumb(lo | (hi << 16), addr, &options());
        Some(Decoded { ins, size })
    } else {
        Some(Decoded { ins: parse_arm(m.u32_at(addr)?, addr, &options()), size: 4 })
    }
}

fn cond_of(ins: &Ins) -> Cond {
    use Ins::*;
    match ins {
        Adc { cond, .. } | Add { cond, .. } | And { cond, .. } | Asr { cond, .. } | B { cond, .. }
        | Bic { cond, .. } | Bl { cond, .. } | Blx { cond, .. } | Bx { cond, .. } | Clz { cond, .. }
        | Cmn { cond, .. } | Cmp { cond, .. } | Eor { cond, .. } | Ldm { cond, .. } | Ldr { cond, .. }
        | Ldrb { cond, .. } | Ldrd { cond, .. } | Ldrh { cond, .. } | Ldrsb { cond, .. } | Ldrsh { cond, .. }
        | Lsl { cond, .. } | Lsr { cond, .. } | Mla { cond, .. } | Mov { cond, .. } | Mrs { cond, .. }
        | Msr { cond, .. } | Mul { cond, .. } | Mvn { cond, .. } | Orr { cond, .. } | Pop { cond, .. }
        | Push { cond, .. } | Ror { cond, .. } | Rsb { cond, .. } | Rsc { cond, .. } | Sbc { cond, .. }
        | Stm { cond, .. } | Str { cond, .. } | Strb { cond, .. } | Strd { cond, .. } | Strh { cond, .. }
        | Sub { cond, .. } | Svc { cond, .. } | Teq { cond, .. } | Tst { cond, .. } | Mcr { cond, .. }
        | Mrc { cond, .. } | Umull { cond, .. } | Umlal { cond, .. } | Smull { cond, .. } | Smlal { cond, .. }
        | Smul { cond, .. } | Smla { cond, .. } | Smulw { cond, .. } | Smlaw { cond, .. }
        | SmlalHalf { cond, .. } | Qadd { cond, .. } | Qsub { cond, .. } | Qdadd { cond, .. }
        | Qdsub { cond, .. } | Swp { cond, .. } | Swpb { cond, .. } | Rrx { cond, .. } | Nop { cond, .. }
        | Ldrt { cond, .. } | Strt { cond, .. } | Ldrbt { cond, .. } | Strbt { cond, .. } => *cond,
        _ => Cond::Al,
    }
}

/// How control leaves an instruction, for the reachability walk
enum Flow {
    Next,
    /// Unconditional exit from the function (return, tail call, computed jump to elsewhere)
    Exit,
    /// Branch to a fixed address
    Branch(u32),
    /// Computed jump inside the function
    Table(Vec<u32>),
}

fn writes_pc(ins: &Ins) -> bool {
    use Ins::*;
    match ins {
        Adc { rd, .. } | Add { rd, .. } | And { rd, .. } | Bic { rd, .. } | Eor { rd, .. } | Mov { rd, .. }
        | Mvn { rd, .. } | Orr { rd, .. } | Rsb { rd, .. } | Rsc { rd, .. } | Sbc { rd, .. } | Sub { rd, .. }
        | Ldr { rd, .. } | Lsl { rd, .. } | Lsr { rd, .. } | Asr { rd, .. } | Ror { rd, .. } => *rd == Reg::Pc,
        Ldm { regs, .. } | Pop { regs, .. } => regs.contains(Reg::Pc),
        Bx { .. } => true,
        _ => false,
    }
}

impl FuncCode {
    /// Finds the bound of a jump table from a `cmp rX, #n` shortly before it
    fn table_bound(&self, addr: u32) -> Option<u32> {
        let mut a = addr;
        for _ in 0..12 {
            let (&prev, d) = self.insns.range(..a).next_back()?;
            if prev + d.size != a {
                return None;
            }
            if let Ins::Cmp { op2: Op2::Imm(Op2Imm { imm, .. }), .. } = d.ins {
                return Some(imm + 1);
            }
            a = prev;
        }
        None
    }
}

fn flow(m: &Module, f: &FuncCode, addr: u32, d: &Decoded) -> Flow {
    use Ins::*;
    let thumb = f.thumb;
    match &d.ins {
        B { target, .. } => Flow::Branch(target.addr),
        // add pc, pc, rX, lsl #2 followed by a row of branches
        Add { rd: Reg::Pc, rn: Reg::Pc, op2: Op2::ShiftImm(ShiftImm { shift_op: ShiftOp::Lsl, imm: 2, .. }), .. }
            if !thumb =>
        {
            let n = f.table_bound(addr).unwrap_or_else(|| {
                (0..256).take_while(|i| matches!(decode(m, addr + 8 + 4 * i, false), Some(Decoded { ins: B { .. }, .. })))
                    .count() as u32
            });
            Flow::Table((0..n).map(|i| addr + 8 + 4 * i).collect())
        }
        // add pc, rX after a table of 16-bit offsets relative to the end of the instruction
        Add { rd: Reg::Pc, rn: Reg::Pc, op2: Op2::ShiftImm(ShiftImm { imm: 0, shift_op: ShiftOp::Lsl, .. }), .. }
            if thumb =>
        {
            let table = addr + 2;
            let mut targets = vec![];
            let bound = f.table_bound(addr);
            let mut i = 0;
            loop {
                if bound.is_some_and(|n| i >= n) {
                    break;
                }
                let Some(e) = m.u16_at(table + 2 * i) else { break };
                let t = (table + 2).wrapping_add(e as u16 as i16 as i32 as u32);
                // The bound may come from an unrelated cmp, so the table also ends at the first entry that can't be
                // one: odd, outside the function, or pointing into the table itself
                if t & 1 != 0 || t < f.start || t >= f.end || t <= table + 2 * i {
                    break;
                }
                targets.push(t);
                i += 1;
            }
            if targets.is_empty() { Flow::Exit } else { Flow::Table(targets) }
        }
        // mov lr, pc / bx rX is a call
        Bx { rm, .. } if !thumb && *rm != Reg::Lr && f.insns.get(&addr.wrapping_sub(4)).is_some_and(|p| {
            matches!(p.ins, Mov { rd: Reg::Lr, op2: Op2::ShiftImm(ShiftImm { rm: Reg::Pc, imm: 0, .. }), .. })
        }) => Flow::Next,
        ins if writes_pc(ins) => Flow::Exit,
        _ => Flow::Next,
    }
}

fn analyze(m: &Module, module: usize, start: u32, end: u32, thumb: bool, extra_roots: &[u32]) -> FuncCode {
    let mut f = FuncCode { module, start, end, thumb, insns: BTreeMap::new(), tables: HashMap::new(),
                           targets: BTreeSet::new() };
    let mut queue: VecDeque<u32> = [start].into_iter().chain(extra_roots.iter().copied()).collect();
    while let Some(addr) = queue.pop_front() {
        if addr < start || addr >= end || f.insns.contains_key(&addr) {
            continue;
        }
        let Some(d) = decode(m, addr, thumb) else { continue };
        let cond = cond_of(&d.ins);
        let next = addr + d.size;
        let fl = flow(m, &f, addr, &d);
        f.insns.insert(addr, d);
        let falls = match fl {
            Flow::Next => true,
            Flow::Exit => cond != Cond::Al,
            Flow::Branch(t) => {
                if t >= start && t < end {
                    f.targets.insert(t);
                    queue.push_back(t);
                }
                cond != Cond::Al
            }
            Flow::Table(ts) => {
                for &t in &ts {
                    f.targets.insert(t);
                    queue.push_back(t);
                }
                f.tables.insert(addr, ts);
                cond != Cond::Al
            }
        };
        if falls {
            queue.push_back(next);
        }
    }
    f
}

// ------------------------------------------------------------------ C emission

struct Program {
    modules: Vec<Module>,
    /// (module, address) -> C function name, for every function and entry that gets code
    names: BTreeMap<(usize, u32), String>,
    /// Entries still to generate: (module, start, end, thumb)
    pending: VecDeque<(usize, u32, u32, bool)>,
    funcs: Vec<FuncCode>,
}

fn fname(m: &Module, addr: u32) -> String {
    format!("f_{}_{:08x}", m.name, addr)
}

impl Program {
    /// The module whose code a call from `from` to `target` resolves to statically: the same module, or a resident
    /// one. Calls into overlays from elsewhere depend on what's loaded, so they go through the runtime.
    fn static_module(&self, from: usize, target: u32) -> Option<usize> {
        if self.modules[from].contains(target) {
            return Some(from);
        }
        self.modules.iter().position(|m| m.overlay.is_none() && m.copy_src.is_none() && m.contains(target))
    }

    /// The C function to call for `target`, creating an entry for it if it starts inside another function
    fn callee(&mut self, from: usize, target: u32) -> Option<String> {
        let mi = self.static_module(from, target)?;
        if let Some(n) = self.names.get(&(mi, target)) {
            return Some(n.clone());
        }
        let (_, thumb, end) = self.modules[mi].func_containing(target)?;
        let name = fname(&self.modules[mi], target);
        self.names.insert((mi, target), name.clone());
        self.pending.push_back((mi, target, end, thumb));
        Some(name)
    }
}

fn r(reg: Reg) -> String {
    format!("c->r[{}]", reg as u8)
}

fn cond_expr(cond: Cond) -> Option<&'static str> {
    Some(match cond {
        Cond::Eq => "c->z",
        Cond::Ne => "!c->z",
        Cond::Hs => "c->c",
        Cond::Lo => "!c->c",
        Cond::Mi => "c->n",
        Cond::Pl => "!c->n",
        Cond::Vs => "c->v",
        Cond::Vc => "!c->v",
        Cond::Hi => "(c->c && !c->z)",
        Cond::Ls => "(!c->c || c->z)",
        Cond::Ge => "(c->n == c->v)",
        Cond::Lt => "(c->n != c->v)",
        Cond::Gt => "(!c->z && c->n == c->v)",
        Cond::Le => "(c->z || c->n != c->v)",
        Cond::Al => return None,
    })
}

struct Emitter<'a> {
    prog: &'a mut Program,
    f_idx: usize,
    out: String,
}

impl Emitter<'_> {
    fn f(&self) -> &FuncCode {
        &self.prog.funcs[self.f_idx]
    }
    fn module(&self) -> &Module {
        &self.prog.modules[self.f().module]
    }
    fn thumb(&self) -> bool {
        self.f().thumb
    }

    /// Value of a register read by the instruction at `addr`
    fn rv(&self, reg: Reg, addr: u32) -> String {
        if reg == Reg::Pc { format!("0x{:08x}u", addr + if self.thumb() { 4 } else { 8 }) } else { r(reg) }
    }
    /// PC as a base address: Thumb rounds it down to a word
    fn base(&self, reg: Reg, addr: u32) -> String {
        if reg == Reg::Pc && self.thumb() { format!("0x{:08x}u", (addr + 4) & !3) } else { self.rv(reg, addr) }
    }

    /// Shifter operand: returns its C expression, writing the shifter carry to `shc` first when `carry` is set
    fn op2(&mut self, op2: &Op2, addr: u32, carry: bool) -> String {
        match op2 {
            Op2::Imm(Op2Imm { imm, rotate_imm }) => {
                if carry {
                    if *rotate_imm != 0 {
                        writeln!(self.out, "    shc = {};", imm >> 31).unwrap();
                    } else {
                        self.out.push_str("    shc = c->c;\n");
                    }
                }
                format!("0x{imm:08x}u")
            }
            Op2::ShiftImm(ShiftImm { rm, shift_op, imm }) => {
                let v = self.rv(*rm, addr);
                let op = match (shift_op, imm) {
                    (ShiftOp::Lsl, 0) => {
                        if carry {
                            self.out.push_str("    shc = c->c;\n");
                        }
                        return v;
                    }
                    (ShiftOp::Ror, 0) => "RRX",
                    (ShiftOp::Lsl, _) => "LSL",
                    (ShiftOp::Lsr, _) => "LSR",
                    (ShiftOp::Asr, _) => "ASR",
                    (ShiftOp::Ror, _) => "ROR",
                };
                if carry { format!("{op}_C(c, {v}, {imm}, &shc)") } else { format!("{op}_C(c, {v}, {imm}, &dummy)") }
            }
            Op2::ShiftReg(ShiftReg { rm, shift_op, rs }) => {
                let v = self.rv(*rm, addr);
                let s = self.rv(*rs, addr);
                let op = match shift_op {
                    ShiftOp::Lsl => "LSLR",
                    ShiftOp::Lsr => "LSRR",
                    ShiftOp::Asr => "ASRR",
                    ShiftOp::Ror => "RORR",
                };
                if carry { format!("{op}_C(c, {v}, {s}, &shc)") } else { format!("{op}_C(c, {v}, {s}, &dummy)") }
            }
        }
    }

    fn shift(&mut self, op: ShiftOp, src: String, amount: &Op2Shift, addr: u32, carry: bool) -> String {
        let (name, n) = match amount {
            Op2Shift::Imm(n) => {
                if *n == 0 && op == ShiftOp::Lsl {
                    if carry {
                        self.out.push_str("    shc = c->c;\n");
                    }
                    return src;
                }
                let name = match op { ShiftOp::Lsl => "LSL", ShiftOp::Lsr => "LSR", ShiftOp::Asr => "ASR",
                                      ShiftOp::Ror => if *n == 0 { "RRX" } else { "ROR" } };
                (name, n.to_string())
            }
            Op2Shift::Reg(rs) => {
                let name = match op { ShiftOp::Lsl => "LSLR", ShiftOp::Lsr => "LSRR", ShiftOp::Asr => "ASRR",
                                      ShiftOp::Ror => "RORR" };
                (name, self.rv(*rs, addr))
            }
        };
        format!("{name}_C(c, {src}, {n}, {})", if carry { "&shc" } else { "&dummy" })
    }

    fn goto_or_exit(&mut self, target: u32, addr: u32) {
        if target >= self.f().start && target < self.f().end && self.f().insns.contains_key(&target) {
            if target <= addr {
                writeln!(self.out, "    TICK(0x{addr:08x}u);").unwrap();
            }
            writeln!(self.out, "    goto L_{target:08x};").unwrap();
        } else {
            self.tail(target);
        }
    }

    /// Branch to a fixed address outside the function: call it, then return to our caller
    fn tail(&mut self, target: u32) {
        let mi = self.f().module;
        match self.prog.callee(mi, target) {
            Some(name) => writeln!(self.out, "    {name}(c); return;").unwrap(),
            None => writeln!(self.out, "    rc_call(c, 0x{target:08x}u); return;").unwrap(),
        }
    }

    fn call(&mut self, target: u32, thumb_target: bool) {
        let mi = self.f().module;
        match self.prog.callee(mi, target) {
            Some(name) => writeln!(self.out, "    {name}(c);").unwrap(),
            None => writeln!(self.out, "    rc_call(c, 0x{:08x}u);", target | thumb_target as u32).unwrap(),
        }
    }

    /// A computed value written to PC: a return, a jump table, or an indirect tail call
    fn pc_write(&mut self, value: &str, addr: u32, is_return: bool) {
        if let Some(ts) = self.f().tables.get(&addr).cloned() {
            writeln!(self.out, "    TICK(0x{addr:08x}u);\n    switch ({value}) {{").unwrap();
            for t in ts.iter().collect::<BTreeSet<_>>() {
                if !self.f().insns.contains_key(t) {
                    continue;
                }
                writeln!(self.out, "    case 0x{t:08x}u: goto L_{t:08x};").unwrap();
            }
            writeln!(self.out, "    default: rc_bad_jump(c, 0x{addr:08x}u, {value}); return;\n    }}").unwrap();
        } else if is_return {
            writeln!(self.out, "    RET({value});").unwrap();
        } else {
            // Jumping to our own return address is a return, e.g. mov r12, lr ... bx r12
            writeln!(self.out, "    if ((({value}) & ~1u) == (ra & ~1u)) return;").unwrap();
            writeln!(self.out, "    rc_call(c, {value}); return;").unwrap();
        }
    }

    fn set_nz(&mut self, v: &str) {
        writeln!(self.out, "    c->n = {v} >> 31; c->z = {v} == 0;").unwrap();
    }

    fn data_proc(&mut self, addr: u32, op: &str, s: bool, rd: Option<Reg>, rn: Option<Reg>, op2: &Op2) {
        let logical = matches!(op, "and" | "eor" | "orr" | "bic" | "mov" | "mvn" | "tst" | "teq");
        let b = self.op2(op2, addr, s && logical);
        let a = rn.map(|rn| if self.thumb() && rn == Reg::Pc && matches!(op2, Op2::Imm(_)) && op == "add" {
            self.base(rn, addr)
        } else {
            self.rv(rn, addr)
        });
        let a = a.unwrap_or_default();
        let expr = match op {
            "and" | "tst" => format!("{a} & {b}"),
            "eor" | "teq" => format!("{a} ^ {b}"),
            "orr" => format!("{a} | {b}"),
            "bic" => format!("{a} & ~{b}"),
            "mov" => b.clone(),
            "mvn" => format!("~{b}"),
            "add" | "cmn" => if s { format!("ADD_F(c, {a}, {b}, 0)") } else { format!("{a} + {b}") },
            "adc" => if s { format!("ADD_F(c, {a}, {b}, c->c)") } else { format!("{a} + {b} + c->c") },
            "sub" | "cmp" => if s { format!("ADD_F(c, {a}, ~{b}, 1)") } else { format!("{a} - {b}") },
            "sbc" => if s { format!("ADD_F(c, {a}, ~{b}, c->c)") } else { format!("{a} - {b} - !c->c") },
            "rsb" => if s { format!("ADD_F(c, {b}, ~{a}, 1)") } else { format!("{b} - {a}") },
            "rsc" => if s { format!("ADD_F(c, {b}, ~{a}, c->c)") } else { format!("{b} - {a} - !c->c") },
            _ => unreachable!(),
        };
        writeln!(self.out, "    t = {expr};").unwrap();
        if s && logical {
            self.set_nz("t");
            self.out.push_str("    c->c = shc;\n");
        }
        match rd {
            Some(Reg::Pc) => {
                if s {
                    // movs pc, lr / subs pc, lr, #4: return from an exception
                    writeln!(self.out, "    c->r[15] = t; rc_exception_return(c); return;").unwrap();
                } else {
                    let ret = op == "mov" && matches!(op2, Op2::ShiftImm(ShiftImm { rm: Reg::Lr, imm: 0, .. }));
                    self.pc_write("t", addr, ret);
                }
            }
            Some(rd) => writeln!(self.out, "    {} = t;", r(rd)).unwrap(),
            None => {}
        }
    }

    fn ldr_str_addr(&mut self, addr: u32, a: &AddrLdrStr) -> (Option<(Reg, String)>, String) {
        let (rn, off, pre, wb) = match a {
            AddrLdrStr::Pre { rn, offset, writeback } => (*rn, offset, true, *writeback),
            AddrLdrStr::Post(AddrLdrStrPost { rn, offset }) => (*rn, offset, false, true),
        };
        let base = self.base(rn, addr);
        let off = match off {
            LdrStrOffset::Imm(i) => format!("{}", *i),
            LdrStrOffset::Reg { subtract, rm, shift_op, imm } => {
                let v = self.rv(*rm, addr);
                let sh = match (shift_op, imm) {
                    (ShiftOp::Lsl, 0) => v,
                    (ShiftOp::Ror, 0) => format!("RRX_C(c, {v}, 0, &dummy)"),
                    (op, n) => format!("{}_C(c, {v}, {n}, &dummy)",
                                       match op { ShiftOp::Lsl => "LSL", ShiftOp::Lsr => "LSR",
                                                  ShiftOp::Asr => "ASR", ShiftOp::Ror => "ROR" }),
                };
                if *subtract { format!("-(int32_t)({sh})") } else { format!("(int32_t)({sh})") }
            }
        };
        writeln!(self.out, "    ea = {base} + (uint32_t)({off});").unwrap();
        let acc = if pre { "ea".to_string() } else { base.clone() };
        (wb.then(|| (rn, "ea".to_string())), acc)
    }

    fn misc_addr(&mut self, addr: u32, a: &AddrMiscLoad) -> (Option<(Reg, String)>, String) {
        let (rn, off, pre, wb) = match a {
            AddrMiscLoad::Pre { rn, offset, writeback } => (*rn, offset, true, *writeback),
            AddrMiscLoad::Post { rn, offset } => (*rn, offset, false, true),
        };
        let base = self.base(rn, addr);
        let off = match off {
            MiscLoadOffset::Imm(i) => format!("{}", *i),
            MiscLoadOffset::Reg { subtract, rm } => {
                let v = self.rv(*rm, addr);
                if *subtract { format!("-(int32_t){v}") } else { format!("(int32_t){v}") }
            }
        };
        writeln!(self.out, "    ea = {base} + (uint32_t)({off});").unwrap();
        let acc = if pre { "ea".to_string() } else { base.clone() };
        (wb.then(|| (rn, "ea".to_string())), acc)
    }

    fn load(&mut self, addr: u32, rd: Reg, wb: Option<(Reg, String)>, value: String) {
        writeln!(self.out, "    t = {value};").unwrap();
        if let Some((rn, v)) = wb {
            if rn != rd {
                writeln!(self.out, "    {} = {v};", r(rn)).unwrap();
            }
        }
        if rd == Reg::Pc {
            let is_ret = self.f().insns[&addr].ins.clone();
            let ret = matches!(is_ret, Ins::Ldr { addr: AddrLdrStr::Post(AddrLdrStrPost { rn: Reg::Sp, .. }), .. });
            self.pc_write("t", addr, ret);
        } else {
            writeln!(self.out, "    {} = t;", r(rd)).unwrap();
        }
    }

    fn store(&mut self, rd: Reg, addr: u32, wb: Option<(Reg, String)>, func: &str, at: String) {
        let v = if rd == Reg::Pc { format!("0x{:08x}u", addr + 12) } else { r(rd) };
        writeln!(self.out, "    {func}({at}, {v});").unwrap();
        if let Some((rn, v)) = wb {
            writeln!(self.out, "    {} = {v};", r(rn)).unwrap();
        }
    }

    fn block(&mut self, addr: u32, mode: LdmStmMode, rn: Reg, wb: bool, regs: RegList, load: bool, user: bool) {
        let n = regs.len();
        let start = match mode {
            LdmStmMode::Ia => "0".to_string(),
            LdmStmMode::Ib => "4".to_string(),
            LdmStmMode::Da => format!("-{}", 4 * n - 4),
            LdmStmMode::Db => format!("-{}", 4 * n),
        };
        let delta: i64 = match mode { LdmStmMode::Ia | LdmStmMode::Ib => 4 * n as i64, _ => -4 * n as i64 };
        writeln!(self.out, "    ea = {} + (uint32_t)({start});", self.rv(rn, addr)).unwrap();
        writeln!(self.out, "    t = {} + (uint32_t)({delta});", self.rv(rn, addr)).unwrap();
        let mut off = 0;
        let mut pc = false;
        for reg in regs.iter() {
            if load {
                if reg == Reg::Pc {
                    writeln!(self.out, "    t2 = rd32(ea + {off});").unwrap();
                    pc = true;
                } else {
                    writeln!(self.out, "    {} = rd32(ea + {off});", r(reg)).unwrap();
                }
            } else {
                let v = if reg == Reg::Pc { format!("0x{:08x}u", addr + 12) } else { r(reg) };
                writeln!(self.out, "    wr32(ea + {off}, {v});").unwrap();
            }
            off += 4;
        }
        if wb && !(load && regs.contains(rn)) {
            writeln!(self.out, "    {} = t;", r(rn)).unwrap();
        }
        if pc {
            if user {
                writeln!(self.out, "    c->r[15] = t2; rc_exception_return(c); return;").unwrap();
            } else {
                self.pc_write("t2", addr, rn == Reg::Sp);
            }
        }
    }

    fn ins(&mut self, addr: u32, d: &Decoded) {
        use Ins::*;
        let thumb = self.thumb();
        let next = addr + d.size;
        let ins = d.ins.clone();
        match &ins {
            Add { s, rd, rn, op2, .. } => self.data_proc(addr, "add", *s, Some(*rd), Some(*rn), op2),
            Adc { s, rd, rn, op2, .. } => self.data_proc(addr, "adc", *s, Some(*rd), Some(*rn), op2),
            Sub { s, rd, rn, op2, .. } => self.data_proc(addr, "sub", *s, Some(*rd), Some(*rn), op2),
            Sbc { s, rd, rn, op2, .. } => self.data_proc(addr, "sbc", *s, Some(*rd), Some(*rn), op2),
            Rsb { s, rd, rn, op2, .. } => self.data_proc(addr, "rsb", *s, Some(*rd), Some(*rn), op2),
            Rsc { s, rd, rn, op2, .. } => self.data_proc(addr, "rsc", *s, Some(*rd), Some(*rn), op2),
            And { s, rd, rn, op2, .. } => self.data_proc(addr, "and", *s, Some(*rd), Some(*rn), op2),
            Eor { s, rd, rn, op2, .. } => self.data_proc(addr, "eor", *s, Some(*rd), Some(*rn), op2),
            Orr { s, rd, rn, op2, .. } => self.data_proc(addr, "orr", *s, Some(*rd), Some(*rn), op2),
            Bic { s, rd, rn, op2, .. } => self.data_proc(addr, "bic", *s, Some(*rd), Some(*rn), op2),
            Mov { s, rd, op2, .. } => self.data_proc(addr, "mov", *s, Some(*rd), None, op2),
            Mvn { s, rd, op2, .. } => self.data_proc(addr, "mvn", *s, Some(*rd), None, op2),
            Cmp { rn, op2, .. } => self.data_proc(addr, "cmp", true, None, Some(*rn), op2),
            Cmn { rn, op2, .. } => self.data_proc(addr, "cmn", true, None, Some(*rn), op2),
            Tst { rn, op2, .. } => self.data_proc(addr, "tst", true, None, Some(*rn), op2),
            Teq { rn, op2, .. } => self.data_proc(addr, "teq", true, None, Some(*rn), op2),
            Neg { rd, rm } => {
                writeln!(self.out, "    {} = ADD_F(c, 0, ~{}, 1);", r(*rd), r(*rm)).unwrap();
            }
            Lsl { s, rd, rn, op2, .. } | Lsr { s, rd, rn, op2, .. } | Asr { s, rd, rn, op2, .. }
            | Ror { s, rd, rn, op2, .. } => {
                let op = match ins { Lsl { .. } => ShiftOp::Lsl, Lsr { .. } => ShiftOp::Lsr, Asr { .. } => ShiftOp::Asr,
                                     _ => ShiftOp::Ror };
                let src = self.rv(*rn, addr);
                let e = self.shift(op, src, op2, addr, *s);
                writeln!(self.out, "    t = {e};").unwrap();
                if *s {
                    self.set_nz("t");
                    self.out.push_str("    c->c = shc;\n");
                }
                if *rd == Reg::Pc { self.pc_write("t", addr, false) } else { writeln!(self.out, "    {} = t;", r(*rd)).unwrap() }
            }
            Rrx { s, rd, rm, .. } => {
                let v = self.rv(*rm, addr);
                writeln!(self.out, "    t = RRX_C(c, {v}, 0, &shc);").unwrap();
                if *s {
                    self.set_nz("t");
                    self.out.push_str("    c->c = shc;\n");
                }
                writeln!(self.out, "    {} = t;", r(*rd)).unwrap();
            }
            Mul { s, rd, rn, rm, .. } => {
                writeln!(self.out, "    t = {} * {};", r(*rn), r(*rm)).unwrap();
                if *s { self.set_nz("t"); }
                writeln!(self.out, "    {} = t;", r(*rd)).unwrap();
            }
            Mla { s, rd, rn, rm, ra, .. } => {
                writeln!(self.out, "    t = {} * {} + {};", r(*rn), r(*rm), r(*ra)).unwrap();
                if *s { self.set_nz("t"); }
                writeln!(self.out, "    {} = t;", r(*rd)).unwrap();
            }
            Umull { s, rd_lo, rd_hi, rn, rm, .. } | Smull { s, rd_lo, rd_hi, rn, rm, .. }
            | Umlal { s, rd_lo, rd_hi, rn, rm, .. } | Smlal { s, rd_lo, rd_hi, rn, rm, .. } => {
                let signed = matches!(ins, Smull { .. } | Smlal { .. });
                let acc = matches!(ins, Umlal { .. } | Smlal { .. });
                let (a, b) = if signed {
                    (format!("(uint64_t)(int64_t)(int32_t){}", r(*rn)), format!("(uint64_t)(int64_t)(int32_t){}", r(*rm)))
                } else {
                    (format!("(uint64_t){}", r(*rn)), format!("(uint64_t){}", r(*rm)))
                };
                let accs = if acc { format!(" + (((uint64_t){} << 32) | {})", r(*rd_hi), r(*rd_lo)) } else { String::new() };
                writeln!(self.out, "    w = {a} * {b}{accs};").unwrap();
                writeln!(self.out, "    {} = (uint32_t)w; {} = (uint32_t)(w >> 32);", r(*rd_lo), r(*rd_hi)).unwrap();
                if *s {
                    writeln!(self.out, "    c->n = (uint32_t)(w >> 63); c->z = w == 0;").unwrap();
                }
            }
            Smul { rd, rn, rn_side, rm, rm_side, .. } | Smla { rd, rn, rn_side, rm, rm_side, .. } => {
                let h = |reg: Reg, side: &RegSide| match side {
                    RegSide::Bottom => format!("(int32_t)(int16_t){}", r(reg)),
                    RegSide::Top => format!("(int32_t)(int16_t)({} >> 16)", r(reg)),
                };
                let p = format!("{} * {}", h(*rn, rn_side), h(*rm, rm_side));
                match &ins {
                    Smla { ra, .. } => writeln!(self.out, "    {} = QFLAG_ADD(c, (uint32_t)({p}), {});", r(*rd), r(*ra)).unwrap(),
                    _ => writeln!(self.out, "    {} = (uint32_t)({p});", r(*rd)).unwrap(),
                }
            }
            Smulw { rd, rn, rm, rm_side, .. } | Smlaw { rd, rn, rm, rm_side, .. } => {
                let h = match rm_side {
                    RegSide::Bottom => format!("(int64_t)(int16_t){}", r(*rm)),
                    RegSide::Top => format!("(int64_t)(int16_t)({} >> 16)", r(*rm)),
                };
                let p = format!("(uint32_t)(((int64_t)(int32_t){} * {h}) >> 16)", r(*rn));
                match &ins {
                    Smlaw { ra, .. } => writeln!(self.out, "    {} = QFLAG_ADD(c, {p}, {});", r(*rd), r(*ra)).unwrap(),
                    _ => writeln!(self.out, "    {} = {p};", r(*rd)).unwrap(),
                }
            }
            SmlalHalf { rd_lo, rd_hi, rn, rn_side, rm, rm_side, .. } => {
                let h = |reg: Reg, side: &RegSide| match side {
                    RegSide::Bottom => format!("(int64_t)(int16_t){}", r(reg)),
                    RegSide::Top => format!("(int64_t)(int16_t)({} >> 16)", r(reg)),
                };
                writeln!(self.out, "    w = (((uint64_t){} << 32) | {}) + (uint64_t)({} * {});",
                         r(*rd_hi), r(*rd_lo), h(*rn, rn_side), h(*rm, rm_side)).unwrap();
                writeln!(self.out, "    {} = (uint32_t)w; {} = (uint32_t)(w >> 32);", r(*rd_lo), r(*rd_hi)).unwrap();
            }
            Qadd { rd, rm, rn, .. } => writeln!(self.out, "    {} = QADD(c, {}, {});", r(*rd), r(*rm), r(*rn)).unwrap(),
            Qsub { rd, rm, rn, .. } => writeln!(self.out, "    {} = QSUB(c, {}, {});", r(*rd), r(*rm), r(*rn)).unwrap(),
            Qdadd { rd, rm, rn, .. } => {
                writeln!(self.out, "    {} = QADD(c, {}, QADD(c, {n}, {n}));", r(*rd), r(*rm), n = r(*rn)).unwrap()
            }
            Qdsub { rd, rm, rn, .. } => {
                writeln!(self.out, "    {} = QSUB(c, {}, QADD(c, {n}, {n}));", r(*rd), r(*rm), n = r(*rn)).unwrap()
            }
            Clz { rd, rm, .. } => writeln!(self.out, "    {} = CLZ({});", r(*rd), r(*rm)).unwrap(),

            Ldr { rd, addr: a, .. } => {
                let (wb, at) = self.ldr_str_addr(addr, a);
                self.load(addr, *rd, wb, format!("rd32r({at})"));
            }
            Ldrb { rd, addr: a, .. } => {
                let (wb, at) = self.ldr_str_addr(addr, a);
                self.load(addr, *rd, wb, format!("rd8({at})"));
            }
            Str { rd, addr: a, .. } => {
                let (wb, at) = self.ldr_str_addr(addr, a);
                self.store(*rd, addr, wb, "wr32", at);
            }
            Strb { rd, addr: a, .. } => {
                let (wb, at) = self.ldr_str_addr(addr, a);
                writeln!(self.out, "    wr8({at}, (uint8_t){});", self.rv(*rd, addr)).unwrap();
                if let Some((rn, v)) = wb {
                    writeln!(self.out, "    {} = {v};", r(rn)).unwrap();
                }
            }
            Ldrt { rd, addr: a, .. } | Ldrbt { rd, addr: a, .. } => {
                let a = AddrLdrStr::Post(a.clone());
                let (wb, at) = self.ldr_str_addr(addr, &a);
                let f = if matches!(ins, Ldrt { .. }) { "rd32r" } else { "rd8" };
                self.load(addr, *rd, wb, format!("{f}({at})"));
            }
            Strt { rd, addr: a, .. } | Strbt { rd, addr: a, .. } => {
                let a = AddrLdrStr::Post(a.clone());
                let (wb, at) = self.ldr_str_addr(addr, &a);
                let f = if matches!(ins, Strt { .. }) { "wr32" } else { "wr8" };
                self.store(*rd, addr, wb, f, at);
            }
            Ldrh { rd, addr: a, .. } | Ldrsh { rd, addr: a, .. } | Ldrsb { rd, addr: a, .. } => {
                let (wb, at) = self.misc_addr(addr, a);
                let v = match ins {
                    Ldrh { .. } => format!("rd16({at})"),
                    Ldrsh { .. } => format!("(uint32_t)(int32_t)(int16_t)rd16({at})"),
                    _ => format!("(uint32_t)(int32_t)(int8_t)rd8({at})"),
                };
                self.load(addr, *rd, wb, v);
            }
            Strh { rd, addr: a, .. } => {
                let (wb, at) = self.misc_addr(addr, a);
                writeln!(self.out, "    wr16({at}, (uint16_t){});", self.rv(*rd, addr)).unwrap();
                if let Some((rn, v)) = wb {
                    writeln!(self.out, "    {} = {v};", r(rn)).unwrap();
                }
            }
            Ldrd { rd, rd2, addr: a, .. } => {
                let (wb, at) = self.misc_addr(addr, a);
                writeln!(self.out, "    t2 = {at};\n    {} = rd32(t2); {} = rd32(t2 + 4);", r(*rd), r(*rd2)).unwrap();
                if let Some((rn, v)) = wb {
                    writeln!(self.out, "    {} = {v};", r(rn)).unwrap();
                }
            }
            Strd { rd, rd2, addr: a, .. } => {
                let (wb, at) = self.misc_addr(addr, a);
                writeln!(self.out, "    t2 = {at};\n    wr32(t2, {}); wr32(t2 + 4, {});", r(*rd), r(*rd2)).unwrap();
                if let Some((rn, v)) = wb {
                    writeln!(self.out, "    {} = {v};", r(rn)).unwrap();
                }
            }
            Swp { rd, rd2, rn, .. } => {
                writeln!(self.out, "    t = rd32r({n}); wr32({n}, {}); {} = t;", r(*rd2), r(*rd), n = r(*rn)).unwrap();
            }
            Swpb { rd, rd2, rn, .. } => {
                writeln!(self.out, "    t = rd8({n}); wr8({n}, (uint8_t){}); {} = t;", r(*rd2), r(*rd), n = r(*rn)).unwrap();
            }
            Ldm { mode, rn, writeback, regs, user_mode, .. } => {
                self.block(addr, *mode, *rn, *writeback, *regs, true, *user_mode)
            }
            Stm { mode, rn, writeback, regs, .. } => self.block(addr, *mode, *rn, *writeback, *regs, false, false),
            Push { regs, .. } => self.block(addr, LdmStmMode::Db, Reg::Sp, true, *regs, false, false),
            Pop { regs, .. } => self.block(addr, LdmStmMode::Ia, Reg::Sp, true, *regs, true, false),

            B { target, .. } => self.goto_or_exit(target.addr, addr),
            Bl { target, .. } => {
                writeln!(self.out, "    c->r[14] = 0x{:08x}u;", next | thumb as u32).unwrap();
                self.call(target.addr, thumb);
            }
            Blx { target: BlxTarget::Direct(t), .. } => {
                writeln!(self.out, "    c->r[14] = 0x{:08x}u;", next | thumb as u32).unwrap();
                // BLX immediate switches state; from Thumb the target is word aligned
                let tgt = if thumb { t.addr & !3 } else { t.addr };
                self.call(tgt, !thumb);
            }
            Blx { target: BlxTarget::Indirect(rm), .. } => {
                writeln!(self.out, "    t = {};\n    c->r[14] = 0x{:08x}u;\n    rc_call(c, t);", self.rv(*rm, addr),
                         next | thumb as u32).unwrap();
            }
            Bx { rm, .. } => {
                // ARM's mov lr, pc / bx rX makes a call
                let prev_is_mov_lr_pc = !thumb && self.f().insns.get(&(addr.wrapping_sub(4))).is_some_and(|p| {
                    matches!(p.ins, Mov { rd: Reg::Lr, op2: Op2::ShiftImm(ShiftImm { rm: Reg::Pc, imm: 0, .. }), .. })
                });
                if *rm == Reg::Lr {
                    writeln!(self.out, "    RET(c->r[14]);").unwrap();
                } else if prev_is_mov_lr_pc {
                    writeln!(self.out, "    rc_call(c, {});", r(*rm)).unwrap();
                } else {
                    let v = self.rv(*rm, addr);
                    self.pc_write(&v, addr, false);
                }
            }

            Mrs { rd, status_reg, .. } => {
                let f = if matches!(status_reg, StatusReg::Cpsr) { "rc_get_cpsr" } else { "rc_get_spsr" };
                writeln!(self.out, "    {} = {f}(c);", r(*rd)).unwrap();
            }
            Msr { status_fields, op2, .. } => {
                let mask = (status_fields.c as u32 * 0xff) | (status_fields.x as u32 * 0xff00)
                    | (status_fields.s as u32 * 0xff0000) | (status_fields.f as u32 * 0xff000000);
                let v = match op2 { MsrOp2::Imm(i) => format!("0x{i:08x}u"), MsrOp2::Reg(rm) => r(*rm) };
                let spsr = matches!(status_fields.reg, StatusReg::Spsr) as u32;
                writeln!(self.out, "    rc_msr(c, {spsr}, 0x{mask:08x}u, {v});").unwrap();
            }
            Mcr { coproc, opc1, rd, crn, crm, opc2, .. } => {
                writeln!(self.out, "    rc_mcr(c, {}, {opc1}, {}, {}, {opc2}, {});", *coproc as u8, *crn as u8,
                         *crm as u8, self.rv(*rd, addr)).unwrap();
            }
            Mrc { coproc, opc1, rd, crn, crm, opc2, .. } => {
                let call = format!("rc_mrc(c, {}, {opc1}, {}, {}, {opc2})", *coproc as u8, *crn as u8, *crm as u8);
                if *rd == Reg::Pc {
                    writeln!(self.out, "    t = {call}; c->n = t >> 31; c->z = (t >> 30) & 1; c->c = (t >> 29) & 1; c->v = (t >> 28) & 1;").unwrap();
                } else {
                    writeln!(self.out, "    {} = {call};", r(*rd)).unwrap();
                }
            }
            Svc { imm, .. } => {
                let n = if thumb { imm & 0xff } else { (imm >> 16) & 0xff };
                writeln!(self.out, "    rc_swi(c, 0x{n:02x}, 0x{addr:08x}u);").unwrap();
            }
            Nop { .. } | Pld { .. } => {}
            Bkpt { .. } | Udf { .. } => writeln!(self.out, "    rc_trap(c, 0x{addr:08x}u);").unwrap(),
            other => {
                writeln!(self.out, "    rc_unimpl(c, 0x{addr:08x}u, \"{:?}\");", other).unwrap();
            }
        }
    }

    fn emit(&mut self) {
        let (name, thumb) = {
            let f = self.f();
            (self.prog.names[&(f.module, f.start)].clone(), f.thumb)
        };
        writeln!(self.out, "void {name}(Cpu *c) {{ /* {} */", if thumb { "thumb" } else { "arm" }).unwrap();
        self.out.push_str("    const uint32_t ra = c->r[14];\n    uint32_t t, t2, ea, shc, dummy; uint64_t w;\n");
        self.out.push_str("    (void)ra; (void)t; (void)t2; (void)ea; (void)shc; (void)dummy; (void)w;\n");
        let insns: Vec<(u32, Decoded)> = self.f().insns.iter().map(|(a, d)| (*a, d.clone())).collect();
        for (addr, d) in insns {
            if self.f().targets.contains(&addr) {
                writeln!(self.out, "L_{addr:08x}:;").unwrap();
            }
            let cond = cond_of(&d.ins);
            match cond_expr(cond) {
                Some(ce) => {
                    writeln!(self.out, "  if ({ce}) {{").unwrap();
                    self.ins(addr, &d);
                    self.out.push_str("  }\n");
                }
                None => self.ins(addr, &d),
            }
            // Falls through to an address the walk didn't decode: past the end of the function, into the next one
            let next = addr + d.size;
            let falls = match flow(self.module(), self.f(), addr, &d) {
                Flow::Next => true,
                _ => cond != Cond::Al,
            };
            if falls && !self.f().insns.contains_key(&next) {
                if next >= self.f().end {
                    self.tail(next);
                } else {
                    writeln!(self.out, "    rc_bad_jump(c, 0x{addr:08x}u, 0x{next:08x}u); return;").unwrap();
                }
            }
        }
        self.out.push_str("}\n\n");
    }
}

// ------------------------------------------------------------------ driver

/// `recomp dis <file> <base> <start> <end> arm|thumb`: print unarm's decode of a range, for reading code that no dsd
/// config covers (the ARM7, copied code)
fn dis(args: &[String]) {
    let data = fs::read(&args[0]).unwrap();
    let (base, start, end, thumb) = (parse_hex(&args[1]), parse_hex(&args[2]), parse_hex(&args[3]), args[4] == "thumb");
    let m = Module { name: String::new(), overlay: None, copy_src: None, base, data, funcs: BTreeMap::new(),
                     labels: vec![] };
    let mut a = start;
    while a < end {
        let Some(d) = decode(&m, a, thumb) else { break };
        let word = if thumb { m.u16_at(a).unwrap() } else { m.u32_at(a).unwrap() };
        println!("{a:08x}  {word:08x}  {:?}", d.ins);
        a += d.size;
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.get(1).is_some_and(|a| a == "dis") {
        return dis(&args[2..]);
    }
    if args.len() < 3 {
        eprintln!("usage: recomp <version> <out_dir> [module ...|all]");
        std::process::exit(2);
    }
    let root = PathBuf::from(".");
    let version = &args[1];
    let out = PathBuf::from(&args[2]);
    let mut which: Vec<String> = args[3..].to_vec();
    if which.is_empty() {
        which = STATIC_MODULES.iter().map(|s| s.to_string()).collect();
    } else if which == ["all"] {
        which = STATIC_MODULES.iter().map(|s| s.to_string()).chain(all_overlays(&root, version)).collect();
    }

    let mut modules: Vec<Module> = which.iter().map(|w| load_module(&root, version, w)).collect();
    let copies = code_copies(&root, version, &modules);
    modules.extend(copies);
    let mut prog = Program { modules, names: BTreeMap::new(), pending: VecDeque::new(), funcs: vec![] };
    for mi in 0..prog.modules.len() {
        let m = &prog.modules[mi];
        let entries: Vec<(u32, bool, u32)> = m.funcs.iter().map(|(&a, &(t, e))| (a, t, e)).collect();
        let labels = m.labels.clone();
        for (a, t, e) in entries {
            prog.names.insert((mi, a), fname(&prog.modules[mi], a));
            prog.pending.push_back((mi, a, e, t));
        }
        for (a, _t) in labels {
            prog.callee(mi, a);
        }
    }

    fs::create_dir_all(&out).unwrap();
    let mut files: Vec<String> = vec![];
    let mut chunk = String::new();
    let mut chunk_funcs = 0;
    let header = "#include \"runtime.h\"\n#include \"funcs.h\"\n\n";
    let mut total_insns = 0usize;
    while let Some((mi, start, end, thumb)) = prog.pending.pop_front() {
        let f = analyze(&prog.modules[mi], mi, start, end, thumb, &[]);
        total_insns += f.insns.len();
        prog.funcs.push(f);
        let idx = prog.funcs.len() - 1;
        let mut em = Emitter { prog: &mut prog, f_idx: idx, out: String::new() };
        em.emit();
        chunk.push_str(&em.out);
        chunk_funcs += 1;
        if chunk.len() > 4 << 20 || chunk_funcs >= 1500 {
            let n = format!("rec_{:03}.c", files.len());
            fs::write(out.join(&n), format!("{header}{chunk}")).unwrap();
            files.push(n);
            chunk.clear();
            chunk_funcs = 0;
        }
        // free the decoded instructions, keeping only what the tables need
        prog.funcs[idx].insns.clear();
    }
    if !chunk.is_empty() {
        let n = format!("rec_{:03}.c", files.len());
        fs::write(out.join(&n), format!("{header}{chunk}")).unwrap();
        files.push(n);
    }

    // Prototypes and the address -> function tables the runtime dispatches through
    let mut h = String::from("#pragma once\n#include \"runtime.h\"\n\n");
    for name in prog.names.values() {
        writeln!(h, "void {name}(Cpu *c);").unwrap();
    }
    fs::write(out.join("funcs.h"), h).unwrap();

    let mut t = String::from("#include \"runtime.h\"\n#include \"funcs.h\"\n\n");
    let mut counts = vec![0usize; prog.modules.len()];
    for (mi, m) in prog.modules.iter().enumerate() {
        let entries: Vec<_> = prog.names.iter().filter(|((fmi, _), _)| *fmi == mi).collect();
        counts[mi] = entries.len();
        if entries.is_empty() {
            continue;
        }
        writeln!(t, "static const RcFunc table_{}[] = {{", m.name).unwrap();
        for ((_, addr), name) in entries {
            writeln!(t, "    {{0x{addr:08x}u, {name}}},").unwrap();
        }
        t.push_str("};\n");
    }
    t.push_str("\nconst RcModule rc_modules[] = {\n");
    for (mi, m) in prog.modules.iter().enumerate() {
        let table = if counts[mi] > 0 { format!("table_{}", m.name) } else { "NULL".to_string() };
        writeln!(t, "    {{\"{}\", {}, 0x{:08x}u, 0x{:08x}u, 0x{:08x}u, {table}, {}}},", m.name,
                 m.overlay.map(|i| i as i64).unwrap_or(-1), m.copy_src.unwrap_or(0), m.base, m.end(),
                 counts[mi]).unwrap();
    }
    writeln!(t, "}};\nconst int rc_module_count = {};", prog.modules.len()).unwrap();
    fs::write(out.join("tables.c"), t).unwrap();

    fs::write(out.join("files.txt"), files.join("\n") + "\n").unwrap();
    eprintln!("recomp: {} modules, {} functions/entries, {} instructions, {} C files", prog.modules.len(),
              prog.names.len(), total_insns, files.len());
}
