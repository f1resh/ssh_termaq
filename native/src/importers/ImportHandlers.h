#pragma once
// Registers bridge handlers for:
//   importer:*  (PuTTY registry / .reg file / ~/.ssh/config / JSON backup preview + apply + export)
//   keys:*      (generate / inspect / convert — PuTTYgen equivalent)
//   knownHosts:importOpenSsh
namespace st {

class Bridge;
class Store;

void registerImportHandlers(Bridge& bridge, Store& store);

}  // namespace st
