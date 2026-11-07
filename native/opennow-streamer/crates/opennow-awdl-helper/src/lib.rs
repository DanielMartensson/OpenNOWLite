use std::collections::HashSet;
use std::io;

pub trait Interface {
    fn is_up(&mut self) -> io::Result<bool>;
    fn set_up(&mut self, up: bool) -> io::Result<()>;
}

pub trait RecoveryStore {
    fn pending(&mut self) -> io::Result<bool>;
    fn mark(&mut self) -> io::Result<()>;
    fn clear(&mut self) -> io::Result<()>;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Operation {
    Acquire,
    Release,
}

impl Operation {
    pub fn decode(byte: u8) -> io::Result<Self> {
        match byte {
            b'A' => Ok(Self::Acquire),
            b'R' => Ok(Self::Release),
            _ => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid operation",
            )),
        }
    }
}

pub fn peer_allowed(peer_uid: u32, console_uid: Option<u32>) -> bool {
    peer_uid != 0 && console_uid == Some(peer_uid)
}

pub fn request<I: Interface, S: RecoveryStore>(
    leases: &mut Leases<I, S>,
    client: u64,
    peer_uid: u32,
    console_uid: Option<u32>,
    frame: [u8; 1],
) -> io::Result<()> {
    if !peer_allowed(peer_uid, console_uid) {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "not the console user",
        ));
    }
    leases.apply(client, Operation::decode(frame[0])?)
}

pub struct Leases<I, S> {
    interface: I,
    store: S,
    clients: HashSet<u64>,
    restore: bool,
}

impl<I: Interface, S: RecoveryStore> Leases<I, S> {
    pub fn recover(mut interface: I, mut store: S) -> io::Result<Self> {
        if store.pending()? {
            interface.set_up(true)?;
            store.clear()?;
        }
        Ok(Self {
            interface,
            store,
            clients: HashSet::new(),
            restore: false,
        })
    }

    pub fn apply(&mut self, client: u64, operation: Operation) -> io::Result<()> {
        match operation {
            Operation::Acquire => {
                if self.clients.contains(&client) {
                    return self.reconcile();
                }
                if self.clients.is_empty() {
                    self.reconcile()?;
                    if self.interface.is_up()? {
                        self.store.mark()?;
                        self.restore = true;
                    }
                }
                self.interface.set_up(false)?;
                self.clients.insert(client);
                Ok(())
            }
            Operation::Release => self.disconnect(client),
        }
    }

    pub fn disconnect(&mut self, client: u64) -> io::Result<()> {
        self.clients.remove(&client);
        self.reconcile()
    }

    pub fn reconcile(&mut self) -> io::Result<()> {
        if !self.clients.is_empty() {
            if self.interface.is_up()? {
                self.interface.set_up(false)?;
            }
        } else if self.restore {
            self.interface.set_up(true)?;
            self.store.clear()?;
            self.restore = false;
        }
        Ok(())
    }

    pub fn shutdown(&mut self) -> io::Result<()> {
        self.clients.clear();
        self.reconcile()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct FakeInterface {
        up: bool,
        writes: Vec<bool>,
        fail: bool,
    }

    impl Interface for FakeInterface {
        fn is_up(&mut self) -> io::Result<bool> {
            Ok(self.up)
        }

        fn set_up(&mut self, up: bool) -> io::Result<()> {
            if self.fail {
                return Err(io::Error::other("interface unavailable"));
            }
            self.up = up;
            self.writes.push(up);
            Ok(())
        }
    }

    #[derive(Default)]
    struct FakeStore {
        pending: bool,
        fail_mark: bool,
        fail_clear: bool,
    }

    impl RecoveryStore for FakeStore {
        fn pending(&mut self) -> io::Result<bool> {
            Ok(self.pending)
        }

        fn mark(&mut self) -> io::Result<()> {
            if self.fail_mark {
                return Err(io::Error::other("journal unavailable"));
            }
            self.pending = true;
            Ok(())
        }

        fn clear(&mut self) -> io::Result<()> {
            if self.fail_clear {
                return Err(io::Error::other("journal unavailable"));
            }
            self.pending = false;
            Ok(())
        }
    }

    fn leases(up: bool) -> Leases<FakeInterface, FakeStore> {
        Leases::recover(
            FakeInterface {
                up,
                ..Default::default()
            },
            FakeStore::default(),
        )
        .unwrap()
    }

    #[test]
    fn acquire_release_and_duplicate_acquire_are_idempotent() {
        let mut state = leases(true);
        state.apply(1, Operation::Acquire).unwrap();
        state.apply(1, Operation::Acquire).unwrap();
        assert!(!state.interface.up);
        assert!(state.store.pending);
        state.apply(1, Operation::Release).unwrap();
        state.apply(1, Operation::Release).unwrap();
        assert!(state.interface.up);
        assert!(!state.store.pending);
        assert_eq!(state.interface.writes, [false, true]);
    }

    #[test]
    fn multiple_clients_and_disconnect_restore_only_after_last_lease() {
        let mut state = leases(true);
        state.apply(1, Operation::Acquire).unwrap();
        state.apply(2, Operation::Acquire).unwrap();
        state.disconnect(1).unwrap();
        assert!(!state.interface.up);
        state.disconnect(2).unwrap();
        assert!(state.interface.up);
    }

    #[test]
    fn originally_down_is_never_forced_up_even_after_os_reenable() {
        let mut state = leases(false);
        state.apply(1, Operation::Acquire).unwrap();
        state.interface.up = true;
        state.reconcile().unwrap();
        state.disconnect(1).unwrap();
        assert!(!state.interface.up);
        assert!(!state.store.pending);
        assert!(state.interface.writes.iter().all(|up| !up));
    }

    #[test]
    fn os_reenabled_interface_is_suppressed_again() {
        let mut state = leases(true);
        state.apply(1, Operation::Acquire).unwrap();
        state.interface.up = true;
        state.reconcile().unwrap();
        assert!(!state.interface.up);
        assert_eq!(state.interface.writes, [false, false]);
        state.shutdown().unwrap();
        assert!(state.interface.up);
    }

    #[test]
    fn restart_restores_persisted_change_before_new_leases() {
        let mut state = leases(true);
        state.apply(1, Operation::Acquire).unwrap();
        let state = Leases::recover(state.interface, state.store).unwrap();
        assert!(state.interface.up);
        assert!(!state.store.pending);
        assert!(state.clients.is_empty());
    }

    #[test]
    fn journal_failure_prevents_interface_change() {
        let mut state = leases(true);
        state.store.fail_mark = true;
        assert!(state.apply(1, Operation::Acquire).is_err());
        assert!(state.interface.up);
        assert!(state.clients.is_empty());
    }

    #[test]
    fn failed_down_keeps_recovery_intent_and_no_lease() {
        let mut state = leases(true);
        state.interface.fail = true;
        assert!(state.apply(1, Operation::Acquire).is_err());
        assert!(state.store.pending);
        assert!(state.clients.is_empty());
        state.interface.fail = false;
        state.reconcile().unwrap();
        assert!(!state.store.pending);
    }

    #[test]
    fn failed_restore_or_clear_keeps_recovery_for_retry() {
        let mut state = leases(true);
        state.apply(1, Operation::Acquire).unwrap();
        state.interface.fail = true;
        assert!(state.disconnect(1).is_err());
        assert!(state.store.pending);
        state.interface.fail = false;
        state.store.fail_clear = true;
        assert!(state.reconcile().is_err());
        assert!(state.restore);
        state.store.fail_clear = false;
        state.reconcile().unwrap();
        assert!(!state.restore);
    }

    #[test]
    fn protocol_is_one_byte_and_accepts_only_acquire_release() {
        assert_eq!(Operation::decode(b'A').unwrap(), Operation::Acquire);
        assert_eq!(Operation::decode(b'R').unwrap(), Operation::Release);
        for byte in 0..=255 {
            if byte != b'A' && byte != b'R' {
                assert!(Operation::decode(byte).is_err());
            }
        }
        let operations: Vec<_> = b"AARR"
            .iter()
            .map(|byte| Operation::decode(*byte).unwrap())
            .collect();
        assert_eq!(
            operations,
            [
                Operation::Acquire,
                Operation::Acquire,
                Operation::Release,
                Operation::Release
            ]
        );
    }

    #[test]
    fn peer_authentication_requires_current_nonroot_console_user() {
        assert!(peer_allowed(501, Some(501)));
        assert!(!peer_allowed(502, Some(501)));
        assert!(!peer_allowed(501, None));
        assert!(!peer_allowed(0, Some(0)));
        assert!(!peer_allowed(501, Some(502)));
    }

    #[test]
    fn invalid_or_unauthorized_frames_do_not_change_interface() {
        let mut state = leases(true);
        assert!(request(&mut state, 1, 502, Some(501), *b"A").is_err());
        assert!(request(&mut state, 1, 501, Some(501), *b"x").is_err());
        assert!(state.interface.writes.is_empty());
        assert!(state.clients.is_empty());
        request(&mut state, 1, 501, Some(501), *b"A").unwrap();
        assert!(request(&mut state, 1, 501, Some(502), *b"A").is_err());
        state.disconnect(1).unwrap();
        assert!(state.interface.up);
    }

    #[cfg(unix)]
    #[test]
    fn socket_frames_remain_bounded_when_coalesced_and_eof_releases_lease() {
        use std::io::{Read, Write};
        use std::os::unix::net::UnixStream;
        let (mut client, mut server) = UnixStream::pair().unwrap();
        let mut state = leases(true);
        client.write_all(b"AARA").unwrap();
        for expected_up in [false, false, true, false] {
            let mut frame = [0];
            server.read_exact(&mut frame).unwrap();
            request(&mut state, 1, 501, Some(501), frame).unwrap();
            server.write_all(b"+").unwrap();
            assert_eq!(state.interface.up, expected_up);
        }
        let mut replies = [0; 4];
        client.read_exact(&mut replies).unwrap();
        assert_eq!(&replies, b"++++");
        drop(client);
        assert_eq!(server.read(&mut [0]).unwrap(), 0);
        state.disconnect(1).unwrap();
        assert!(state.interface.up);
    }
}
