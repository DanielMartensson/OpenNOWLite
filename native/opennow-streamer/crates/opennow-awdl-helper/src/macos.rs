use opennow_awdl_helper::{Interface, Leases, RecoveryStore, peer_allowed, request};
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

const SOCKET_DIR: &str = "/var/run/opennow-awdl";
const SOCKET: &str = "/var/run/opennow-awdl/control.sock";
const STATE_DIR: &str = "/var/db/opennow-awdl";
const JOURNAL: &str = "/var/db/opennow-awdl/restore";
const MAX_CLIENTS: usize = 32;
const SIOCGIFFLAGS: libc::c_ulong =
    0xc000_0000 | (std::mem::size_of::<libc::ifreq>() as libc::c_ulong) << 16 | 0x6911;
const SIOCSIFFLAGS: libc::c_ulong =
    0x8000_0000 | (std::mem::size_of::<libc::ifreq>() as libc::c_ulong) << 16 | 0x6910;
static STOP: AtomicBool = AtomicBool::new(false);

extern "C" fn stop(_: libc::c_int) {
    STOP.store(true, Ordering::Relaxed);
}

fn checked(result: libc::c_int) -> io::Result<()> {
    if result == -1 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

fn socket(domain: libc::c_int, kind: libc::c_int) -> io::Result<OwnedFd> {
    let fd = unsafe { libc::socket(domain, kind, 0) };
    checked(fd)?;
    let fd = unsafe { OwnedFd::from_raw_fd(fd) };
    checked(unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_SETFD, libc::FD_CLOEXEC) })?;
    Ok(fd)
}

struct Awdl(OwnedFd);

impl Awdl {
    fn flags(&self) -> io::Result<libc::ifreq> {
        let mut request: libc::ifreq = unsafe { std::mem::zeroed() };
        for (destination, source) in request.ifr_name.iter_mut().zip(b"awdl0") {
            *destination = *source as libc::c_char;
        }
        checked(unsafe { libc::ioctl(self.0.as_raw_fd(), SIOCGIFFLAGS, &mut request) })?;
        Ok(request)
    }
}

impl Interface for Awdl {
    fn is_up(&mut self) -> io::Result<bool> {
        let request = self.flags()?;
        Ok(unsafe { request.ifr_ifru.ifru_flags } as libc::c_int & libc::IFF_UP != 0)
    }

    fn set_up(&mut self, up: bool) -> io::Result<()> {
        let mut request = self.flags()?;
        let flags = unsafe { request.ifr_ifru.ifru_flags };
        request.ifr_ifru.ifru_flags = if up {
            flags | libc::IFF_UP as i16
        } else {
            flags & !(libc::IFF_UP as i16)
        };
        checked(unsafe { libc::ioctl(self.0.as_raw_fd(), SIOCSIFFLAGS, &request) })
    }
}

fn secure_directory(path: &str, mode: u32) -> io::Result<()> {
    match fs::create_dir(path) {
        Ok(()) => fs::set_permissions(path, fs::Permissions::from_mode(mode))?,
        Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {}
        Err(error) => return Err(error),
    }
    let metadata = fs::symlink_metadata(path)?;
    if !metadata.is_dir() || metadata.uid() != 0 || metadata.mode() & 0o777 != mode {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "unsafe helper directory",
        ));
    }
    Ok(())
}

struct Journal;

impl Journal {
    fn sync_directory() -> io::Result<()> {
        File::open(STATE_DIR)?.sync_all()
    }
}

impl RecoveryStore for Journal {
    fn pending(&mut self) -> io::Result<bool> {
        let mut file = match OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NOFOLLOW)
            .open(JOURNAL)
        {
            Ok(file) => file,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(false),
            Err(error) => return Err(error),
        };
        let metadata = file.metadata()?;
        if !metadata.is_file()
            || metadata.uid() != 0
            || metadata.mode() & 0o777 != 0o600
            || metadata.len() != 1
        {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "unsafe recovery journal",
            ));
        }
        let mut byte = [0];
        file.read_exact(&mut byte)?;
        if byte != *b"1" {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid recovery journal",
            ));
        }
        Ok(true)
    }

    fn mark(&mut self) -> io::Result<()> {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW)
            .open(JOURNAL)?;
        file.write_all(b"1")?;
        file.sync_all()?;
        Self::sync_directory()
    }

    fn clear(&mut self) -> io::Result<()> {
        match fs::remove_file(JOURNAL) {
            Ok(()) => {}
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            Err(error) => return Err(error),
        }
        Self::sync_directory()
    }
}

fn console_uid() -> Option<u32> {
    fs::metadata("/dev/console")
        .ok()
        .map(|metadata| metadata.uid())
        .filter(|uid| *uid != 0)
}

fn peer_uid(stream: &UnixStream) -> io::Result<u32> {
    let mut uid = 0;
    let mut gid = 0;
    checked(unsafe { libc::getpeereid(stream.as_raw_fd(), &mut uid, &mut gid) })?;
    Ok(uid)
}

struct Client {
    stream: UnixStream,
    uid: u32,
    id: u64,
}

pub fn run() -> io::Result<()> {
    if unsafe { libc::geteuid() } != 0 || std::env::args_os().len() != 1 {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "requires root and no arguments",
        ));
    }
    secure_directory(STATE_DIR, 0o700)?;
    let lock = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW)
        .open(format!("{STATE_DIR}/lock"))?;
    checked(unsafe { libc::flock(lock.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) })?;
    let interface = Awdl(socket(libc::AF_INET, libc::SOCK_DGRAM)?);
    let mut leases = Leases::recover(interface, Journal)?;
    secure_directory(SOCKET_DIR, 0o755)?;
    if Path::new(SOCKET).exists() {
        fs::remove_file(SOCKET)?;
    }
    let listener = UnixListener::bind(SOCKET)?;
    fs::set_permissions(SOCKET, fs::Permissions::from_mode(0o666))?;
    listener.set_nonblocking(true)?;
    let route = socket(libc::PF_ROUTE, libc::SOCK_RAW)?;
    checked(unsafe { libc::fcntl(route.as_raw_fd(), libc::F_SETFL, libc::O_NONBLOCK) })?;
    unsafe {
        libc::signal(libc::SIGTERM, stop as *const () as libc::sighandler_t);
        libc::signal(libc::SIGINT, stop as *const () as libc::sighandler_t);
        libc::signal(libc::SIGPIPE, libc::SIG_IGN);
    }
    let result = serve(&listener, &route, &mut leases);
    let recovery = leases.shutdown();
    let _ = fs::remove_file(SOCKET);
    result.and(recovery)
}

fn serve(
    listener: &UnixListener,
    route: &OwnedFd,
    leases: &mut Leases<Awdl, Journal>,
) -> io::Result<()> {
    let mut clients: Vec<Client> = Vec::new();
    let mut next_id = 1_u64;
    while !STOP.load(Ordering::Relaxed) {
        let mut descriptors = vec![
            libc::pollfd {
                fd: listener.as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            },
            libc::pollfd {
                fd: route.as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            },
        ];
        descriptors.extend(clients.iter().map(|client| libc::pollfd {
            fd: client.stream.as_raw_fd(),
            events: libc::POLLIN,
            revents: 0,
        }));
        let result = unsafe {
            libc::poll(
                descriptors.as_mut_ptr(),
                descriptors.len() as libc::nfds_t,
                1000,
            )
        };
        if result == -1 && io::Error::last_os_error().kind() != io::ErrorKind::Interrupted {
            return Err(io::Error::last_os_error());
        }
        let console = console_uid();
        for index in (0..clients.len()).rev() {
            let client = &mut clients[index];
            let events = descriptors[index + 2].revents;
            let mut remove = !peer_allowed(client.uid, console);
            if !remove && events & libc::POLLIN != 0 {
                let mut byte = [0];
                match client.stream.read(&mut byte) {
                    Ok(1) => {
                        let result = request(leases, client.id, client.uid, console, byte);
                        if let Err(error) = &result {
                            eprintln!("AWDL lease: {error}");
                        }
                        let reply = if result.is_ok() { b'+' } else { b'-' };
                        remove = client.stream.write_all(&[reply]).is_err() || result.is_err();
                    }
                    Err(error) if error.kind() == io::ErrorKind::WouldBlock => {}
                    _ => remove = true,
                }
            }
            remove |= events & (libc::POLLHUP | libc::POLLERR | libc::POLLNVAL) != 0;
            if remove {
                let client = clients.swap_remove(index);
                if let Err(error) = leases.disconnect(client.id) {
                    eprintln!("AWDL disconnect recovery: {error}");
                }
            }
        }
        if descriptors[0].revents & libc::POLLIN != 0 {
            for _ in 0..MAX_CLIENTS {
                match listener.accept() {
                    Ok((stream, _)) => {
                        let uid = peer_uid(&stream)?;
                        if clients.len() < MAX_CLIENTS && peer_allowed(uid, console) {
                            stream.set_nonblocking(true)?;
                            clients.push(Client {
                                stream,
                                uid,
                                id: next_id,
                            });
                            next_id = next_id
                                .checked_add(1)
                                .ok_or_else(|| io::Error::other("client id exhausted"))?;
                        }
                    }
                    Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                    Err(error) => {
                        eprintln!("AWDL accept: {error}");
                        break;
                    }
                }
            }
        }
        if descriptors[1].revents & libc::POLLIN != 0 {
            let mut buffer = [0_u8; 4096];
            for _ in 0..32 {
                if unsafe {
                    libc::read(route.as_raw_fd(), buffer.as_mut_ptr().cast(), buffer.len())
                } <= 0
                {
                    break;
                }
            }
        }
        leases.reconcile()?;
        if result > 0 {
            std::thread::sleep(std::time::Duration::from_millis(100));
        }
    }
    Ok(())
}
