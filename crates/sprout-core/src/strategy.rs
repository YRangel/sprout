//! Interception strategy selection (ADR-0002).

use crate::elf::GuestClass;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Strategy {
    /// LD_PRELOAD in-process translation. Fast: zero syscall stops.
    Preload,
    /// ptrace-based translation. Required for static/Go binaries.
    Ptrace,
}

impl Strategy {
    /// Route from ELF classification. The available guest loader location is
    /// decided later (see `Rootfs::guest_loader`); this is pure capability
    /// routing.
    pub fn for_elf(class: &GuestClass) -> Option<Self> {
        match class {
            GuestClass::Dynamic { .. } => Some(Strategy::Preload),
            // Direct-syscall images (static, Go-dynamic) need the
            // supervisor: the interposer can't see their path arguments.
            GuestClass::Static | GuestClass::GoDynamic { .. } => Some(Strategy::Ptrace),
            GuestClass::NotElf | GuestClass::Elf32 => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn routes_dynamic_to_preload() {
        let c = GuestClass::Dynamic {
            interp: "/lib/ld.so".into(),
        };
        assert_eq!(Strategy::for_elf(&c), Some(Strategy::Preload));
    }

    #[test]
    fn routes_static_to_ptrace() {
        assert_eq!(
            Strategy::for_elf(&GuestClass::Static),
            Some(Strategy::Ptrace)
        );
        // Go-dynamic is equally supervisor-routed (raw syscalls).
        let g = GuestClass::GoDynamic {
            interp: "/lib/ld.so".into(),
        };
        assert_eq!(Strategy::for_elf(&g), Some(Strategy::Ptrace));
    }

    #[test]
    fn rejects_scripts_and_32bit() {
        assert_eq!(Strategy::for_elf(&GuestClass::NotElf), None);
        assert_eq!(Strategy::for_elf(&GuestClass::Elf32), None);
    }
}
