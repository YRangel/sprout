//! sprout-core: rootfs model, ELF inspection, strategy selection, launch plan.
//!
//! Pipeline: parse flags → build [`Rootfs`] → classify the guest program's
//! ELF header ([`elf::classify`]) → pick [`Strategy`] → build a
//! [`plan::LaunchPlan`] → `exec`. The launcher never guesses: every
//! transformation is testable before a process ever starts.

mod elf;
mod error;
mod plan;
mod rootfs;
mod strategy;

pub mod sanitize;

pub use elf::{classify, elf_meta, ElfError, GuestClass};
pub use error::Error;
pub use plan::LaunchPlan;
pub use rootfs::{Binding, LibcFlavor, Rootfs};
pub use strategy::Strategy;

pub type Result<T> = std::result::Result<T, Error>;
