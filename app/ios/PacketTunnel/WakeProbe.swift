import Darwin
import Foundation
import Network

final class WakeProbe {
    private struct Probe {
        let name: String
        let fd: Int32
        let connected: Bool
    }

    private let queue = DispatchQueue(label: "vigil.wakeprobe")
    private let host: String
    private let port: UInt16
    private var target = sockaddr_in()
    private var probes = [Probe]()
    private var sources = [DispatchSourceRead]()
    private var connections = [NWConnection]()
    private var paired: UInt16 = 0
    private var anchors = [Int32]()

    static func configured() -> WakeProbe? {
        guard let value = Bundle.main.object(forInfoDictionaryKey: "VigilWakeProbe") as? String,
              let colon = value.lastIndex(of: ":"),
              let port = UInt16(value[value.index(after: colon)...]) else {
            return nil
        }

        return WakeProbe(host: String(value[..<colon]), port: port)
    }

    private init?(host: String, port: UInt16) {
        self.host = host
        self.port = port

        target.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        target.sin_family = sa_family_t(AF_INET)
        target.sin_port = port.bigEndian

        guard inet_pton(AF_INET, host, &target.sin_addr) == 1 else {
            NSLog("vigil: probe needs an IPv4 address, not '%@'", host)
            return nil
        }
    }

    func start() {
        queue.async {
            NSLog("vigil: probe talking to %@:%d", self.host, Int(self.port))

            self.open("plain", to: nil) { _ in true }
            self.open("connected", to: self.port) { _ in true }
            self.open("bound", to: nil) { fd in self.bindLocal(fd) }
            self.open("pair", to: self.port) { fd in self.share(fd) }
            self.open("anchor", to: nil) { fd in self.anchor(fd) }

            if self.port < UInt16.max {
                self.open("pair2", to: self.port + 1) { fd in self.share(fd) }
            }

            self.open("voice", to: nil) { fd in
                var type = NET_SERVICE_TYPE_VO
                return setsockopt(fd, SOL_SOCKET, SO_NET_SERVICE_TYPE,
                                  &type, socklen_t(MemoryLayout<Int32>.size)) == 0
            }

            for probe in self.probes {
                self.send(probe, Data("hello \(probe.name)".utf8))
            }

            self.network()
        }
    }

    func stop() {
        queue.async {
            for source in self.sources { source.cancel() }
            self.sources.removeAll()

            for fd in self.anchors { close(fd) }
            self.anchors.removeAll()
            self.probes.removeAll()

            for connection in self.connections { connection.cancel() }
            self.connections.removeAll()
        }
    }

    private func open(_ name: String, to remote: UInt16?, _ setup: (Int32) -> Bool) {
        let fd = socket(AF_INET, SOCK_DGRAM, 0)
        guard fd >= 0 else {
            NSLog("vigil: probe %@ socket: %@", name, String(cString: strerror(errno)))
            return
        }

        guard setup(fd) else {
            NSLog("vigil: probe %@ setup: %@", name, String(cString: strerror(errno)))
            close(fd)
            return
        }

        if let remote {
            var sin = target
            sin.sin_port = remote.bigEndian
            let ret = withUnsafePointer(to: &sin) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }

            guard ret == 0 else {
                NSLog("vigil: probe %@ connect: %@", name, String(cString: strerror(errno)))
                close(fd)
                return
            }
        }

        _ = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)

        let probe = Probe(name: name, fd: fd, connected: remote != nil)
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)

        source.setEventHandler { [weak self] in self?.read(probe) }
        source.setCancelHandler { close(fd) }
        source.resume()

        sources.append(source)
        probes.append(probe)
    }

    private func share(_ fd: Int32) -> Bool {
        var on: Int32 = 1
        guard setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on,
                         socklen_t(MemoryLayout<Int32>.size)) == 0 else {
            return false
        }

        var local = sockaddr_in()
        local.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        local.sin_family = sa_family_t(AF_INET)
        local.sin_port = paired.bigEndian

        let bound = withUnsafePointer(to: &local) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bound == 0 else { return false }

        if paired == 0 {
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            let named = withUnsafeMutablePointer(to: &local) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    getsockname(fd, $0, &len)
                }
            }
            guard named == 0 else { return false }

            paired = UInt16(bigEndian: local.sin_port)
            NSLog("vigil: probe pair on local port %d", Int(paired))
        }

        return true
    }

    private func anchor(_ fd: Int32) -> Bool {
        var on: Int32 = 1
        let size = socklen_t(MemoryLayout<sockaddr_in>.size)

        var local = sockaddr_in()
        local.sin_len = UInt8(size)
        local.sin_family = sa_family_t(AF_INET)

        guard withUnsafePointer(to: &local, {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, size) }
        }) == 0 else {
            return false
        }

        guard setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, socklen_t(MemoryLayout<Int32>.size)) == 0 else {
            return false
        }

        var len = size
        guard withUnsafeMutablePointer(to: &local, {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &len) }
        }) == 0 else {
            return false
        }

        let anchor = socket(AF_INET, SOCK_DGRAM, 0)
        guard anchor >= 0 else { return false }

        guard setsockopt(anchor, SOL_SOCKET, SO_REUSEPORT, &on, socklen_t(MemoryLayout<Int32>.size)) == 0,
              withUnsafePointer(to: &local, {
                  $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(anchor, $0, size) }
              }) == 0 else {
            NSLog("vigil: probe anchor bind: %@", String(cString: strerror(errno)))
            close(anchor)
            return true
        }

        var discard = local
        discard.sin_addr.s_addr = UInt32(0xc0000201).bigEndian
        discard.sin_port = UInt16(9).bigEndian

        guard withUnsafePointer(to: &discard, {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(anchor, $0, size) }
        }) == 0 else {
            NSLog("vigil: probe anchor connect: %@", String(cString: strerror(errno)))
            close(anchor)
            return true
        }

        NSLog("vigil: probe anchor on local port %d", Int(UInt16(bigEndian: local.sin_port)))
        anchors.append(anchor)

        return true
    }

    private func bindLocal(_ fd: Int32) -> Bool {
        let route = socket(AF_INET, SOCK_DGRAM, 0)
        guard route >= 0 else { return false }
        defer { close(route) }

        var sin = target
        let routed = withUnsafePointer(to: &sin) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(route, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard routed == 0 else { return false }

        var local = sockaddr_in()
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        let named = withUnsafeMutablePointer(to: &local) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                getsockname(route, $0, &len)
            }
        }
        guard named == 0 else { return false }

        local.sin_port = 0

        var addr = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        inet_ntop(AF_INET, &local.sin_addr, &addr, socklen_t(INET_ADDRSTRLEN))
        NSLog("vigil: probe bound to %@", String(cString: addr))

        return withUnsafePointer(to: &local) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        } == 0
    }

    private func read(_ probe: Probe) {
        let size = 128
        var buf = [UInt8](repeating: 0, count: size)

        while true {
            let ret = recv(probe.fd, &buf, size, 0)
            guard ret > 0 else { return }

            if let reply = answer(probe.name, Data(buf[0..<ret])) {
                send(probe, reply)
            }
        }
    }

    private func send(_ probe: Probe, _ data: Data) {
        var sin = target

        let ret = data.withUnsafeBytes { bytes -> Int in
            if probe.connected {
                return Darwin.send(probe.fd, bytes.baseAddress, bytes.count, 0)
            }

            return withUnsafePointer(to: &sin) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    sendto(probe.fd, bytes.baseAddress, bytes.count, 0,
                           $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }

        if ret < 0 {
            NSLog("vigil: probe %@ send: %@", probe.name, String(cString: strerror(errno)))
        }
    }

    private func network() {
        guard let endpoint = NWEndpoint.Port(rawValue: port) else { return }

        let params = NWParameters.udp
        params.allowLocalEndpointReuse = true

        let connection = NWConnection(host: NWEndpoint.Host(host), port: endpoint, using: params)

        connection.stateUpdateHandler = { [weak self, weak connection] state in
            guard let self, let connection else { return }

            NSLog("vigil: probe nw %@", "\(state)")

            guard case .ready = state else { return }

            connection.send(content: Data("hello nw".utf8), completion: .contentProcessed { _ in })

            guard case .hostPort(let local, let lport)? = connection.currentPath?.localEndpoint else {
                NSLog("vigil: probe nw has no local endpoint: %@",
                      "\(String(describing: connection.currentPath))")
                return
            }

            self.shared(local, lport)
        }

        connections.append(connection)
        connection.start(queue: queue)
        receive(connection, "nw")
    }

    private func shared(_ local: NWEndpoint.Host, _ lport: NWEndpoint.Port) {
        guard connections.count == 1, port < UInt16.max,
              let endpoint = NWEndpoint.Port(rawValue: port + 1) else {
            return
        }

        let params = NWParameters.udp
        params.allowLocalEndpointReuse = true
        params.requiredLocalEndpoint = .hostPort(host: local, port: lport)

        let connection = NWConnection(host: NWEndpoint.Host(host), port: endpoint, using: params)

        NSLog("vigil: probe shared from %@ port %d", "\(local)", Int(lport.rawValue))

        connection.stateUpdateHandler = { [weak connection] state in
            NSLog("vigil: probe shared %@", "\(state)")

            guard case .ready = state else { return }

            connection?.send(content: Data("hello shared".utf8),
                             completion: .contentProcessed { _ in })
        }

        connections.append(connection)
        connection.start(queue: queue)
        receive(connection, "shared")
    }

    private func receive(_ connection: NWConnection, _ name: String) {
        connection.receiveMessage { [weak self] data, _, _, error in
            guard let self else { return }

            if let data, let reply = self.answer(name, data) {
                connection.send(content: reply, completion: .contentProcessed { _ in })
            }

            if let error {
                NSLog("vigil: probe %@ receive: %@", name, "\(error)")
                return
            }

            self.receive(connection, name)
        }
    }

    private func answer(_ name: String, _ data: Data) -> Data? {
        guard let text = String(data: data, encoding: .utf8) else { return nil }

        let parts = text.split(separator: " ")
        guard parts.count == 3, parts[0] == "ping", parts[1] == name else { return nil }

        NSLog("vigil: probe %@ ping %@", name, String(parts[2]))

        return Data("pong \(name) \(parts[2])".utf8)
    }
}
