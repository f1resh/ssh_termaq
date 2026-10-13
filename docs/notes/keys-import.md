# keys-import module notes

Owner: keys-import agent. Owns `native/src/{keys,putty,importers}/**`, `native/tests/{keys,putty,fixtures}/**`.

## Status checklist (resume pointer)

- [ ] keys/SshWire: SSH wire-format reader/writer
- [ ] keys/Ppk: PPK v2/v3 parse, MAC verify, decrypt (aes256-cbc; SHA1 KDF v2 / Argon2 v3), writer v2/v3
- [ ] keys/OpenSshKey: openssh-key-v1 parse/write, PPK <-> OpenSSH private field mapping
- [ ] keys/KeyLoader: detectKeyFormat, loadPrivateKey (3 passphrase attempts)
- [ ] keys/Keygen: generateKey, inspectKey, convertKey
- [ ] putty/RegFile: .reg parser/writer
- [ ] putty/PuttyRegistry: HKCU reader, key-name escaping
- [ ] putty/PuttyMapping: PuTTY values <-> SessionConfig
- [ ] putty/PuttyHostKeys: SshHostKeys -> KnownHost
- [ ] putty/PuttyImport: previews + .reg export
- [ ] importers/SshConfig
- [ ] importers/KnownHostsFile (OpenSSH known_hosts)
- [ ] importers/JsonBackup
- [ ] importers/ImportApply
- [ ] importers/ImportHandlers (all importer:*, keys:*, knownHosts:importOpenSsh)
- [ ] tests: keys (PPK, writer, plink cross-check, OpenSSH fixtures, keygen/inspect/convert)
- [ ] tests: putty (.reg, mapping, host keys, export roundtrip, ssh config, known_hosts, apply, [.registry])

## Requests

(none yet)
