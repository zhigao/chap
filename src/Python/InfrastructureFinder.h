// Copyright (c) 2020 VMware, Inc. All Rights Reserved.
// SPDX-License-Identifier: GPL-2.0

#pragma once
#include <algorithm>
#include "../ModuleDirectory.h"
#include "../VirtualAddressMap.h"
#include "../VirtualMemoryPartition.h"
#include "TypeDirectory.h"

namespace chap {
namespace Python {
template <typename Offset>
class InfrastructureFinder {
 public:
  static constexpr Offset TYPE_IN_PYOBJECT = sizeof(Offset);
  static constexpr Offset PYTHON2_MASK_IN_DICT = 4 * sizeof(Offset);
  static constexpr Offset PYTHON2_KEYS_IN_DICT = 5 * sizeof(Offset);
  static constexpr Offset PYTHON2_TRIPLES_IN_DICT_KEYS = 0;
  static constexpr Offset PYTHON2_CSTRING_IN_STR = 0x24;
  static constexpr Offset PYTHON3_KEYS_IN_DICT = 3 * sizeof(Offset);
  static constexpr Offset PYTHON3_CAPACITY_IN_DICT_KEYS = sizeof(Offset);
  static constexpr Offset PYTHON3_TRIPLES_IN_DICT_KEYS = 4 * sizeof(Offset);
  static constexpr Offset PYTHON3_CSTRING_IN_STR = 6 * sizeof(Offset);
  static constexpr Offset PYTHON2_GARBAGE_COLLECTION_HEADER_SIZE =
      4 * sizeof(Offset);
  static constexpr Offset PYTHON3_GARBAGE_COLLECTION_HEADER_SIZE =
      3 * sizeof(Offset);
  static constexpr Offset LENGTH_IN_STR = 2 * sizeof(Offset);
  static constexpr Offset UNKNOWN_OFFSET = ~0;
  enum MajorVersion { Version2, Version3, VersionUnknownOrOther };
  InfrastructureFinder(const ModuleDirectory<Offset>& moduleDirectory,
                       VirtualMemoryPartition<Offset>& partition,
                       TypeDirectory<Offset>& typeDirectory)
      : PYTHON_ARENA("python arena"),
        _moduleDirectory(moduleDirectory),
        _majorVersion(VersionUnknownOrOther),
        _isResolved(false),
        _virtualMemoryPartition(partition),
        _virtualAddressMap(partition.GetAddressMap()),
        _typeDirectory(typeDirectory),
        _arenaOffset(0),
        _poolsLimitOffset(_arenaOffset + sizeof(Offset)),
        _numFreePoolsOffset(_poolsLimitOffset + sizeof(Offset)),
        _maxPoolsOffset(_numFreePoolsOffset + sizeof(uint32_t)),
        _availablePoolsOffset(_maxPoolsOffset + sizeof(uint32_t)),
        _nextOffset(_availablePoolsOffset + sizeof(Offset)),
        _prevOffset(_nextOffset + sizeof(Offset)),
        _arenaStructSize(_prevOffset + sizeof(Offset)),
        _numArenas(0),
        _arenaStructArray(0),
        _arenaStructCount(0),
        _arenaStructArrayLimit(0),
        _arenaSize(0),
        _poolSize(0),
        _maxPoolsIfAligned(0),
        _maxPoolsIfNotAligned(0),
        _allArenasAreAligned(true),
        _typeType(0),
        _typeSize(0),
        _baseInType(UNKNOWN_OFFSET),
        _objectType(0),
        _dictInType(UNKNOWN_OFFSET),
        _getSetInType(UNKNOWN_OFFSET),
        _dictType(0),
        _keysInDict(UNKNOWN_OFFSET),
        _triplesInDictKeys(UNKNOWN_OFFSET),
        _strType(0),
        _cstringInStr(UNKNOWN_OFFSET),
        _garbageCollectionHeaderSize(0),
        _cachedKeysInHeapTypeObject(UNKNOWN_OFFSET) {}
  void Resolve() {
    if (_isResolved) {
      abort();
    }
    if (!_moduleDirectory.IsResolved()) {
      abort();
    }

    typename ModuleDirectory<Offset>::const_iterator itLib =
        _moduleDirectory.end();
    typename ModuleDirectory<Offset>::const_iterator itExe =
        _moduleDirectory.end();
    for (typename ModuleDirectory<Offset>::const_iterator it =
             _moduleDirectory.begin();
         it != _moduleDirectory.end(); ++it) {
      if (it->first.find("libpython") != std::string::npos) {
        itLib = it;
        _libraryPath = it->first;
        break;
      }
      if ((it->first.find("/python") != std::string::npos) ||
          (it->first.find("python") == 0)) {
        itExe = it;
        _executablePath = it->first;
      }
    }
    if (itLib != _moduleDirectory.end()) {
      if (_libraryPath.find("libpython3") != std::string::npos) {
        _majorVersion = Version3;
      } else if (_libraryPath.find("libpython2") != std::string::npos) {
        _majorVersion = Version2;
      }
    }
    if (itExe != _moduleDirectory.end()) {
      if (_executablePath.find("python3") != std::string::npos) {
        switch (_majorVersion) {
          case Version2:
            std::cerr << "Warning: version derived from executable conflicts "
                         "with one "
                         "from library\n"
                         "Please raise an issue (at "
                         "https://github.com/vmware/chap).\n";
            _majorVersion = VersionUnknownOrOther;
            break;
          case Version3:
            break;
          case VersionUnknownOrOther:
            _majorVersion = Version3;
            break;
        }
      }
      if (_executablePath.find("python2") != std::string::npos) {
        switch (_majorVersion) {
          case Version2:
            break;
          case Version3:
            std::cerr << "Warning: version derived from executable conflicts "
                         "with one "
                         "from library\n"
                         "Please raise an issue (at "
                         "https://github.com/vmware/chap).\n";
            _majorVersion = VersionUnknownOrOther;
            break;
          case VersionUnknownOrOther:
            _majorVersion = Version2;
            break;
        }
      }
    }

    if (itLib != _moduleDirectory.end()) {
      FindArenaStructArrayAndTypes(itLib->second);
    }
    if (itExe != _moduleDirectory.end()) {
      if (_arenaStructArray == 0) {
        FindArenaStructArrayAndTypes(itExe->second);
      }
    }
    _isResolved = true;
  }

  bool IsResolved() const { return _isResolved; }
  Offset ArenaStructFor(Offset candidateAddressInArena) const {
    if (_activeIndices.empty()) {
      return 0;
    }

    std::vector<uint32_t>::size_type leftToCheck = _activeIndices.size();
    const uint32_t* remainingToCheck = &(_activeIndices[0]);

    Reader reader(_virtualAddressMap);
    while (leftToCheck != 0) {
      std::vector<uint32_t>::size_type halfLeftToCheck = leftToCheck / 2;
      const uint32_t* nextToCheck = remainingToCheck + halfLeftToCheck;
      Offset arenaStruct =
          _arenaStructArray + ((*nextToCheck) * _arenaStructSize);
      Offset arena = reader.ReadOffset(arenaStruct);
      if ((arena + _arenaSize) <= candidateAddressInArena) {
        remainingToCheck = ++nextToCheck;
        leftToCheck -= (halfLeftToCheck + 1);
      } else {
        if (arena <= candidateAddressInArena) {
          return arenaStruct;
        }
        leftToCheck = halfLeftToCheck;
      }
    }
    return 0;
  }

  const std::string& LibraryPath() const { return _libraryPath; }
  Offset ArenaOffset() const { return _arenaOffset; }
  Offset PoolsLimitOffset() const { return _poolsLimitOffset; }
  Offset NumFreePoolsOffset() const { return _numFreePoolsOffset; }
  Offset MaxPoolsOffset() const { return _maxPoolsOffset; }
  Offset AvailablePoolsOffset() const { return _availablePoolsOffset; }
  Offset NextOffset() const { return _nextOffset; }
  Offset PrevOffset() const { return _prevOffset; }
  Offset ArenaStructSize() const { return _arenaStructSize; }
  Offset NumArenas() const { return _numArenas; }
  Offset ArenaStructArray() const { return _arenaStructArray; }
  Offset ArenaStructCount() const { return _arenaStructCount; }
  Offset ArenaStructArrayLimit() const { return _arenaStructArrayLimit; }
  Offset ArenaSize() const { return _arenaSize; }
  const std::vector<uint32_t>& ActiveIndices() const { return _activeIndices; }
  Offset PoolSize() const { return _poolSize; }
  Offset MaxPoolsIfAligned() const { return _maxPoolsIfAligned; }
  Offset MaxPoolsIfNotAligned() const { return _maxPoolsIfNotAligned; }
  bool AllArenasAreAligned() const { return _allArenasAreAligned; }
  Offset TypeType() const { return _typeType; }
  Offset TypeSize() const { return _typeSize; }
  Offset BaseInType() const { return _baseInType; }
  Offset ObjectType() const { return _objectType; }
  Offset DictInType() const { return _dictInType; }
  Offset DictType() const { return _dictType; }
  Offset KeysInDict() const { return _keysInDict; }
  Offset TriplesInDictKeys() const { return _triplesInDictKeys; }
  Offset StrType() const { return _strType; }
  Offset CstringInStr() const { return _cstringInStr; }
  const std::vector<Offset> NonEmptyGarbageCollectionLists() const {
    return _nonEmptyGarbageCollectionLists;
  }
  const Offset GarbageCollectionHeaderSize() const {
    return _garbageCollectionHeaderSize;
  }
  const Offset CachedKeysInHeapTypeObject() const {
    return _cachedKeysInHeapTypeObject;
  }
  const char* PYTHON_ARENA;

  const std::string& GetTypeName(Offset typeObject) const {
    return _typeDirectory.GetTypeName(typeObject);
  }

  bool HasType(Offset typeObject) const {
    return _typeDirectory.HasType(typeObject);
  }

  bool IsATypeType(Offset typeObject) const {
    int depth = 0;
    Reader reader(_virtualAddressMap);
    while (typeObject != 0) {
      if (typeObject == _typeType) {
        return true;
      }
      if (++depth == 100) {
        /*
         * This branch is not expected ever to be taken because it is assumed
         * that there is reasonable expectation that typeObject will be the
         * address of a type object and that depth will not be anywhere near
         * that much.
         */
        std::cerr
            << "Warning: excessive depth found for probable type object 0x"
            << std::hex << typeObject << ".\n";
        break;
      }
      typeObject = reader.ReadOffset(typeObject + _baseInType, 0);
    }
    return false;
  }

 private:
  typedef typename VirtualAddressMap<Offset>::Reader Reader;
  typedef typename VirtualAddressMap<Offset>::RangeAttributes RangeAttributes;
  const ModuleDirectory<Offset>& _moduleDirectory;
  MajorVersion _majorVersion;
  std::string _libraryPath;
  std::string _executablePath;
  bool _isResolved;
  VirtualMemoryPartition<Offset>& _virtualMemoryPartition;
  const VirtualAddressMap<Offset>& _virtualAddressMap;
  TypeDirectory<Offset>& _typeDirectory;
  const Offset _arenaOffset = 0;
  const Offset _poolsLimitOffset;
  const Offset _numFreePoolsOffset;
  const Offset _maxPoolsOffset;
  const Offset _availablePoolsOffset;
  const Offset _nextOffset;
  const Offset _prevOffset;
  const Offset _arenaStructSize;
  Offset _numArenas;
  Offset _arenaStructArray;
  Offset _arenaStructCount;
  Offset _arenaStructArrayLimit;
  Offset _arenaSize;
  Offset _poolSize;
  Offset _maxPoolsIfAligned;
  Offset _maxPoolsIfNotAligned;
  bool _allArenasAreAligned;
  Offset _typeType;
  Offset _typeSize;
  Offset _baseInType;
  Offset _objectType;
  Offset _dictInType;
  Offset _getSetInType;
  Offset _dictType;
  Offset _keysInDict;
  Offset _triplesInDictKeys;
  Offset _strType;
  Offset _cstringInStr;
  std::vector<uint32_t> _activeIndices;
  std::vector<Offset> _nonEmptyGarbageCollectionLists;
  Offset _garbageCollectionHeaderSize;
  Offset _cachedKeysInHeapTypeObject;

  void FindMajorVersionFromPaths() {}
  void FindArenaStructArrayAndTypes(
      const typename ModuleDirectory<Offset>::RangeToFlags& rangeToFlags) {
    Reader moduleReader(_virtualAddressMap);
    Reader reader(_virtualAddressMap);

    Offset bestBase = 0;
    Offset bestLimit = 0;
    for (typename ModuleDirectory<Offset>::RangeToFlags::const_iterator
             itRange = rangeToFlags.begin();
         itRange != rangeToFlags.end(); ++itRange) {
      int flags = itRange->_value;
      if ((flags & RangeAttributes::IS_WRITABLE) != 0) {
        Offset base = itRange->_base;
        /*
         * At present the module finding logic can get a lower value for the
         * limit than the true limit.  It is conservative about selecting the
         * limit to avoid tagging too large a range in the partition.  However
         * this conservative estimate is problematic if the pointer to the
         * arena struct array lies between the calculated limit and the real
         * limit.  This code works around this to extend the limit to the
         * last consecutive byte that has the same permission as the last
         * byte in the range.
         */
        Offset limit = _virtualAddressMap.find(itRange->_limit - 1).Limit();

        for (Offset moduleAddr = base; moduleAddr < limit;
             moduleAddr += sizeof(Offset)) {
          Offset arenaStruct0 = moduleReader.ReadOffset(moduleAddr, 0xbad);
          if ((arenaStruct0 & (sizeof(Offset) - 1)) != 0) {
            continue;
          }
          if (arenaStruct0 == 0) {
            continue;
          }

          Offset arena0 = reader.ReadOffset(arenaStruct0, 0xbad);
          if (arena0 == 0 || (arena0 & (sizeof(Offset) - 1)) != 0) {
            /*
             * The very first arena won't ever be given back, because
             * some of those allocations will be needed pretty much
             * forever.
             */
            continue;
          }
          Offset poolsLimit0 =
              reader.ReadOffset(arenaStruct0 + _poolsLimitOffset, 0xbad);
          if ((poolsLimit0 & 0xfff) != 0 || poolsLimit0 < arena0) {
            continue;
          }

          uint32_t numFreePools0 =
              reader.ReadU32(arenaStruct0 + _numFreePoolsOffset, 0xbad);
          uint32_t maxPools0 =
              reader.ReadU32(arenaStruct0 + _maxPoolsOffset, 0xbad);
          if (maxPools0 == 0 || numFreePools0 > maxPools0) {
            continue;
          }
          Offset numNeverUsedPools0 = numFreePools0;

          Offset firstAvailablePool =
              reader.ReadOffset(arenaStruct0 + _availablePoolsOffset, 0xbad);
          if (firstAvailablePool != 0) {
            Offset availablePool = firstAvailablePool;
            for (; availablePool != 0;
                 availablePool = reader.ReadOffset(
                     availablePool + 2 * sizeof(Offset), 0xbad)) {
              if ((availablePool & 0xfff) != 0) {
                break;
              }
              if (numNeverUsedPools0 == 0) {
                break;
              }
              --numNeverUsedPools0;
            }
            if (availablePool != 0) {
              continue;
            }
          }

          Offset poolSize =
              ((poolsLimit0 - arena0) / (maxPools0 - numNeverUsedPools0)) &
              ~0xfff;

          if (poolSize == 0) {
            continue;
          }

          if ((poolsLimit0 & (poolSize - 1)) != 0) {
            continue;
          }

          Offset arenaSize = maxPools0 * poolSize;
          if ((arena0 & (poolSize - 1)) != 0) {
            arenaSize += poolSize;
          }
          Offset maxPoolsIfAligned = arenaSize / poolSize;
          Offset maxPoolsIfNotAligned = maxPoolsIfAligned - 1;

          Offset arenaStruct = arenaStruct0 + _arenaStructSize;
          bool freeListTrailerFound = false;
          for (;; arenaStruct += _arenaStructSize) {
            Offset arena = reader.ReadOffset(arenaStruct, 0xbad);
            Offset nextArenaStruct =
                reader.ReadOffset(arenaStruct + _nextOffset, 0xbad);
            if (arena == 0) {
              /*
               * The arena is not allocated.  The only live field other
               * than the address is the next pointer, which is
               * constrained to be either null or a pointer to an
               * element in the array.
               */
              if (nextArenaStruct != 0) {
                /*
                 * This pointer is constrained to either be 0 or to put
                 * to somewhere in the array of arena structs.
                 */
                if (nextArenaStruct < arenaStruct0) {
                  break;
                }
                if (((nextArenaStruct - arenaStruct0) % _arenaStructSize) !=
                    0) {
                  break;
                }
              } else {
                if (freeListTrailerFound) {
                  break;
                }
                freeListTrailerFound = true;
              }
            } else {
              /*
               * The arena is allocated.  We can't really evaluate the
               * next unless the prev is also set because the next
               * may be residue from before the arena was allocated.
               */
              uint32_t numFreePools =
                  reader.ReadU32(arenaStruct + _numFreePoolsOffset, 0xbad);
              uint32_t maxPools =
                  reader.ReadU32(arenaStruct + _maxPoolsOffset, 0xbad);
              if (maxPools != (((arena & (poolSize - 1)) == 0)
                                   ? maxPoolsIfAligned
                                   : maxPoolsIfNotAligned)) {
                break;
              }
              if (numFreePools > maxPools) {
                break;
              }
              Offset poolsLimit =
                  reader.ReadOffset(arenaStruct + _poolsLimitOffset, 0xbad);
              if (poolsLimit < arena || poolsLimit > (arena + arenaSize) ||
                  (poolsLimit & (poolSize - 1)) != 0) {
                break;
              }

              /*
               * Note that we don't bother to check the next and prev
               * links for arena structs with allocated arena structs
               * because the links are live only for arenas that still
               * are considered usable for allocations.
               */
            }
          }
          Offset arenaStructArrayLimit = arenaStruct;
          for (arenaStruct -= _arenaStructSize; arenaStruct > arenaStruct0;
               arenaStruct -= _arenaStructSize) {
            if (reader.ReadOffset(arenaStruct, 0xbad) == 0 &&
                reader.ReadOffset(arenaStruct + _nextOffset, 0xbad) >
                    arenaStructArrayLimit) {
              arenaStructArrayLimit = arenaStruct;
            }
          }
          Offset numValidArenaStructs =
              (arenaStructArrayLimit - arenaStruct0) / _arenaStructSize;
          if (_arenaStructCount < numValidArenaStructs) {
            _arenaStructCount = numValidArenaStructs;
            _arenaStructArray = arenaStruct0;
            _arenaStructArrayLimit = arenaStructArrayLimit;
            _poolSize = poolSize;
            _arenaSize = arenaSize;
            _maxPoolsIfAligned = maxPoolsIfAligned;
            _maxPoolsIfNotAligned = maxPoolsIfNotAligned;
            bestBase = base;
            bestLimit = limit;
          }
        }
      }
    }
    for (Offset arenaStruct = _arenaStructArray;
         arenaStruct < _arenaStructArrayLimit;
         arenaStruct += _arenaStructSize) {
      Offset arena = reader.ReadOffset(arenaStruct + _arenaOffset, 0);
      if (arena != 0) {
        _numArenas++;
        if ((arena & (_poolSize - 1)) != 0) {
          _allArenasAreAligned = false;
        }
      }
    }
    _activeIndices.reserve(_numArenas);
    for (Offset arenaStruct = _arenaStructArray;
         arenaStruct < _arenaStructArrayLimit;
         arenaStruct += _arenaStructSize) {
      Offset arena = reader.ReadOffset(arenaStruct + _arenaOffset, 0);
      if (arena != 0) {
        _activeIndices.push_back((arenaStruct - _arenaStructArray) /
                                 _arenaStructSize);
        /*
         * Attempt to claim the arena.  We do not treat it as an anchor area
         * because it is a source of allocations.
         */
        if (_allArenasAreAligned &&
            !_virtualMemoryPartition.ClaimRange(arena, _arenaSize, PYTHON_ARENA,
                                                false)) {
          std::cerr << "Warning: Python arena at 0x" << std::hex << arena
                    << " was already marked as something else.\n";
        }
      }
    }
    std::sort(_activeIndices.begin(), _activeIndices.end(), [&](uint32_t i0,
                                                                uint32_t i1) {
      return reader.ReadOffset(this->_arenaStructArray +
                                   ((Offset)(i0)) * this->_arenaStructSize,
                               0xbad) <
             reader.ReadOffset(this->_arenaStructArray +
                                   ((Offset)(i1)) * this->_arenaStructSize,
                               0xbad);
    });
    if (_arenaStructCount != 0) {
      FindTypes(bestBase, bestLimit, reader);
      if (_typeType != 0) {
        FindNonEmptyGarbageCollectionLists(bestBase, bestLimit, reader);
        FindDynamicallyAllocatedTypes();
      }
    }
  }

  /*
   * This is not as expensive as it looks, as it normally converges within the
   * first 10 blocks in the first pool of the first arena.
   */
  void FindTypes(Offset base, Offset limit, Reader& reader) {
    if (_majorVersion == VersionUnknownOrOther) {
      /*
       * At present this could happen in the case of a statically linked
       * python where chap also is not able to derive the correct name of
       * the main executable or in the very unusual case that an older
       * version was being used.  Derivation of the main executable name
       * works for cores generated by reasonably recent versions of gdb
       * where the module paths are in the PT_NOTE section, but some
       * improvement could be made for the older case.  At some point
       * python4 will exist.
       */
      std::cerr << "Warning: the major version of python was not derived "
                   "successfully from module paths.\n";
      std::cerr << "An attempt will be made to derive needed offsets.\n";
    }
    for (Offset arenaStruct = _arenaStructArray;
         arenaStruct < _arenaStructArrayLimit;
         arenaStruct += _arenaStructSize) {
      Offset arena = reader.ReadOffset(arenaStruct + _arenaOffset, 0);
      if (arena == 0) {
        continue;
      }
      Offset firstPool = (arena + (_poolSize - 1)) & ~(_poolSize - 1);
      Offset poolsLimit = (arena + _arenaSize) & ~(_poolSize - 1);
      for (Offset pool = firstPool; pool < poolsLimit; pool += _poolSize) {
        if (reader.ReadU32(pool, 0) == 0) {
          continue;
        }
        Offset blockSize = _poolSize - reader.ReadU32(pool + 0x2c, 0);
        if (blockSize == 0) {
          continue;
        }
        Offset poolLimit = pool + _poolSize;
        for (Offset block = pool + 0x30; block + blockSize <= poolLimit;
             block += blockSize) {
          Offset candidateType =
              reader.ReadOffset(block + TYPE_IN_PYOBJECT, 0xbadbad);
          if ((candidateType & (sizeof(Offset) - 1)) != 0) {
            continue;
          }
          Offset candidateTypeType =
              reader.ReadOffset(candidateType + 8, 0xbadbad);
          if ((candidateTypeType & (sizeof(Offset) - 1)) != 0) {
            continue;
          }
          if (candidateTypeType !=
              reader.ReadOffset(candidateTypeType + TYPE_IN_PYOBJECT, 0)) {
            continue;
          }
          if (candidateTypeType < base || candidateTypeType >= limit) {
            continue;
          }
          Offset typeSize =
              reader.ReadOffset(candidateTypeType + 4 * sizeof(Offset), ~0);
          if (limit - candidateTypeType < typeSize) {
            continue;
          }
          Offset baseInType = 0x18 * sizeof(Offset);
          for (; baseInType < typeSize - 0x10; baseInType += sizeof(Offset)) {
            Offset candidateObjType =
                reader.ReadOffset(candidateTypeType + baseInType, 0xbad);
            if ((candidateObjType & (sizeof(Offset) - 1)) != 0) {
              continue;
            }
            Offset candidateDict = reader.ReadOffset(
                candidateTypeType + baseInType + sizeof(Offset), 0xbad);
            if ((candidateDict & (sizeof(Offset) - 1)) != 0) {
              continue;
            }
            if (reader.ReadOffset(candidateObjType + TYPE_IN_PYOBJECT, 0) !=
                candidateTypeType) {
              continue;
            }

            if (reader.ReadOffset(candidateObjType + baseInType, 0xbad) != 0) {
              continue;
            }
            Offset candidateDictType =
                reader.ReadOffset(candidateDict + TYPE_IN_PYOBJECT, 0);
            if (reader.ReadOffset(candidateDictType + TYPE_IN_PYOBJECT,
                                  0xbad) != candidateTypeType) {
              continue;
            }
            if (reader.ReadOffset(candidateDictType + baseInType, 0xbad) !=
                candidateObjType) {
              continue;
            }
            _typeType = candidateTypeType;
            _typeSize = typeSize;
            _baseInType = baseInType;
            _objectType = candidateObjType;
            _dictInType = baseInType + sizeof(Offset);
            _getSetInType = baseInType - sizeof(Offset);
            _dictType = candidateDictType;
            _typeDirectory.RegisterType(_typeType, "type");
            _typeDirectory.RegisterType(_objectType, "object");
            _typeDirectory.RegisterType(_dictType, "dict");

            /*
             * The dict for the type type is non-empty and contains multiple
             * string keys.  This allows deriving or checking offsets
             * associated with dict and with str.
             */
            if (!CalculateOffsetsForDictAndStr(candidateDict)) {
              return;
            }

            FindStaticallyAllocatedTypes(base, limit, reader);

            Offset builtinDict = 0;
            if (_keysInDict == PYTHON3_KEYS_IN_DICT) {
              builtinDict = FindPython3Builtins(base, limit);
            } else if (_keysInDict == PYTHON2_KEYS_IN_DICT) {
              builtinDict = FindPython2Builtins(base, limit);
            }
            if (builtinDict != 0) {
              RegisterBuiltinTypesFromDict(builtinDict);
            }
            return;
          }
        }
      }
    }
  }

  void FindDynamicallyAllocatedTypes() {
    bool needHtCachedKeysOffset = (_majorVersion != Version2);
    Reader reader(_virtualAddressMap);
    Reader otherReader(_virtualAddressMap);
    for (auto listHead : _nonEmptyGarbageCollectionLists) {
      Offset prevNode = listHead;
      for (Offset node = reader.ReadOffset(listHead, listHead);
           node != listHead; node = reader.ReadOffset(node, listHead)) {
        if (reader.ReadOffset(node + sizeof(Offset), 0) != prevNode) {
          std::cerr << "Warning: GC list at 0x" << std::hex << listHead
                    << " is ill-formed near 0x" << node << ".\n";
          break;
        }
        prevNode = node;
        Offset typeCandidate = node + _garbageCollectionHeaderSize;
        if (_typeDirectory.HasType(typeCandidate)) {
          continue;
        }
        if (IsATypeType(
                reader.ReadOffset(typeCandidate + TYPE_IN_PYOBJECT, 0))) {
          _typeDirectory.RegisterType(typeCandidate, "");
          if (needHtCachedKeysOffset) {
            for (Offset keysOffset = _typeSize - 0x10 * sizeof(Offset);
                 keysOffset < _typeSize; keysOffset += sizeof(Offset)) {
              Offset keysCandidate =
                  reader.ReadOffset(typeCandidate + keysOffset, 0xbad);
              if ((keysCandidate & (sizeof(Offset) - 1)) != 0) {
                continue;
              }
              if (otherReader.ReadOffset(keysCandidate, 0) != 1) {
                /*
                 * This is not true of PyDictKeysObject in general, because the
                 * ref count can quite easily be something other than 1, but it
                 * happens to be true for most of the ones that are referenced
                 * from type objects and in fact we need just one to figure
                 * out the offset.
                 */
                continue;
              }
              Offset size =
                  otherReader.ReadOffset(keysCandidate + sizeof(Offset), 0);
              if (size == 0 || ((size | (size - 1)) != (size ^ (size - 1)))) {
                continue;
              }
              Offset usable = otherReader.ReadOffset(
                  keysCandidate + 3 * sizeof(Offset), 0xbad);
              if (size - 1 != usable) {
                continue;
              }
              if (usable < otherReader.ReadOffset(
                               keysCandidate + 4 * sizeof(Offset), ~0)) {
                continue;
              }
              _cachedKeysInHeapTypeObject = keysOffset;
              needHtCachedKeysOffset = false;
              break;
            }
          }
        }
      }
    }
  }

  void FindStaticallyAllocatedTypes(Offset base, Offset limit, Reader& reader) {
    Offset candidateLimit = limit - _typeSize + 1;
    Offset candidate = base;
    Reader baseTypeReader(_virtualAddressMap);
    while (candidate < candidateLimit) {
      if (!_typeDirectory.HasType(candidate) &&
          reader.ReadOffset(candidate + TYPE_IN_PYOBJECT, 0xbad) == _typeType) {
        Offset baseType = reader.ReadOffset(candidate + _baseInType, 0);
        if (baseType != 0) {
          if (baseType == _objectType ||
              (_typeDirectory.HasType(baseType) ||
               baseTypeReader.ReadOffset(baseType + TYPE_IN_PYOBJECT, 0) ==
                   _typeType)) {
            _typeDirectory.RegisterType(candidate, "");
            candidate += _baseInType;
            continue;
          }
        } else if (candidate != _objectType) {
          /*
           * For python 3, at least type "object" has no base type, but that
           * is OK because at this point we have already located the
           * corresponding type object.  For Python 2, there are other types
           * that do not inherit from anything, including at least cell,
           * methoddescriptor and classmethoddescriptor.
           */
          Offset getSet = reader.ReadOffset(candidate + _getSetInType, 0);
          if (getSet >= base && getSet < limit) {
            _typeDirectory.RegisterType(candidate, "");
          }
        }
      }
      candidate += sizeof(Offset);
    }
  }

  /*
   * The following function attempts to use the specified built-in dict
   * to determine names for any built-in types for which the name was
   * statically allocated and didn't make it into the core.  This can
   * happen because it is not uncommon for gdb to not keep images for
   * things that can be obtained from the main executable or from
   * shared libraries.
   */
  void RegisterBuiltinTypesFromDict(Offset builtinDict) {
    Reader reader(_virtualAddressMap);
    Offset keys = reader.ReadOffset(builtinDict + _keysInDict, 0xbad);

    if ((keys & (sizeof(Offset) - 1)) != 0) {
      return;
    }

    Offset capacity =
        (_triplesInDictKeys == 0)
            ? (reader.ReadOffset(builtinDict + PYTHON2_MASK_IN_DICT, ~0) + 1)
            : reader.ReadOffset(keys + PYTHON3_CAPACITY_IN_DICT_KEYS, ~0);
    Offset triples = keys + _triplesInDictKeys;
    Offset triplesLimit = triples + capacity * 3 * sizeof(Offset);
    for (Offset triple = triples; triple < triplesLimit;
         triple += (3 * sizeof(Offset))) {
      Offset key = reader.ReadOffset(triple + sizeof(Offset), 0);
      if (key == 0) {
        continue;
      }
      Offset value = reader.ReadOffset(triple + 2 * sizeof(Offset), 0);
      if (value == 0) {
        continue;
      }
      const char* image;
      Offset numBytesFound =
          _virtualAddressMap.FindMappedMemoryImage(key, &image);
      if (numBytesFound < _cstringInStr + 2) {
        continue;
      }
      if (*((Offset*)(image + TYPE_IN_PYOBJECT)) != _strType) {
        continue;
      }
      Offset length = *((Offset*)(image + LENGTH_IN_STR));
      if (numBytesFound < _cstringInStr + length + 1) {
        continue;
      }
      if (reader.ReadOffset(value + TYPE_IN_PYOBJECT, 0) != _typeType) {
        continue;
      }
      _typeDirectory.RegisterType(value, image + _cstringInStr);
    }
  }

  Offset FindPython3Builtins(Offset base, Offset limit) {
    Reader reader(_virtualAddressMap);
    Reader dictReader(_virtualAddressMap);
    for (Offset dictRefCandidate = base; dictRefCandidate < limit;
         dictRefCandidate += sizeof(Offset)) {
      Offset dictCandidate = reader.ReadOffset(dictRefCandidate, 0xbad);
      if ((dictCandidate & (sizeof(Offset) - 1)) != 0) {
        continue;
      }
      if (dictReader.ReadOffset(dictCandidate + TYPE_IN_PYOBJECT, 0xbad) !=
          _dictType) {
        continue;
      }
      Offset keys = dictReader.ReadOffset(dictCandidate + _keysInDict, 0xbad);

      if ((keys & (sizeof(Offset) - 1)) != 0) {
        continue;
      }
      Offset capacity =
          dictReader.ReadOffset(keys + PYTHON3_CAPACITY_IN_DICT_KEYS, ~0);
      if (capacity >= 0x200) {
        // We don't expect that many built-ins.
        continue;
      }

      Offset firstValue = keys + _triplesInDictKeys + 2 * sizeof(Offset);

      Offset valuesLimit = firstValue + capacity * 3 * sizeof(Offset);
      bool foundTypeType = false;
      bool foundObjectType = false;
      bool foundDictType = false;
      for (Offset o = firstValue; o < valuesLimit; o += (3 * sizeof(Offset))) {
        Offset typeCandidate = dictReader.ReadOffset(o, 0xbad);
        if (typeCandidate == _typeType) {
          foundTypeType = true;
        } else if (typeCandidate == _objectType) {
          foundObjectType = true;
        } else if (typeCandidate == _dictType) {
          foundDictType = true;
        }
      }
      if (foundTypeType && foundObjectType && foundDictType) {
        return dictCandidate;
      }
    }
    return 0;
  }
  Offset FindPython2Builtins(Offset base, Offset limit) {
    Reader reader(_virtualAddressMap);
    Reader dictReader(_virtualAddressMap);
    for (Offset dictRefCandidate = base; dictRefCandidate < limit;
         dictRefCandidate += sizeof(Offset)) {
      Offset outerDictCandidate = reader.ReadOffset(dictRefCandidate, 0xbad);
      if ((outerDictCandidate & (sizeof(Offset) - 1)) != 0) {
        continue;
      }
      if (dictReader.ReadOffset(outerDictCandidate + TYPE_IN_PYOBJECT, 0xbad) !=
          _dictType) {
        continue;
      }
      Offset keys =
          dictReader.ReadOffset(outerDictCandidate + _keysInDict, 0xbad);

      if ((keys & (sizeof(Offset) - 1)) != 0) {
        continue;
      }
      Offset mask =
          dictReader.ReadOffset(outerDictCandidate + PYTHON2_MASK_IN_DICT, ~0);
      if (mask == (Offset)(~0)) {
        continue;
      }
      Offset capacity = mask + 1;
      Offset firstKey = keys + _triplesInDictKeys + sizeof(Offset);
      Offset keysLimit = firstKey + capacity * 3 * sizeof(Offset);
      Offset builtinDict = 0;
      for (Offset o = firstKey; o < keysLimit; o += (3 * sizeof(Offset))) {
        Offset dictCandidate = dictReader.ReadOffset(o + sizeof(Offset), 0xbad);
        if (dictCandidate == 0) {
          continue;
        }
        if (dictReader.ReadOffset(dictCandidate + TYPE_IN_PYOBJECT, 0xbad) !=
            _dictType) {
          continue;
        }
        Offset strCandidate = dictReader.ReadOffset(o, 0xbad);
        if (strCandidate == 0) {
          continue;
        }
        if ((strCandidate & (sizeof(Offset) - 1)) != 0) {
          continue;
        }
        const char* image;
        Offset numBytesFound =
            _virtualAddressMap.FindMappedMemoryImage(strCandidate, &image);
        if (numBytesFound < _cstringInStr + 12) {
          continue;
        }
        if (!strcmp("__builtin__", image + _cstringInStr)) {
          builtinDict = dictCandidate;
        }
      }
      if (builtinDict != 0) {
        return builtinDict;
      }
    }
    return 0;
  }
  bool CalculateOffsetsForDictAndStr(Offset dictForTypeType) {
    bool succeeded = true;
    switch (_majorVersion) {
      case Version2:
        _keysInDict = PYTHON2_KEYS_IN_DICT;
        _triplesInDictKeys = PYTHON2_TRIPLES_IN_DICT_KEYS;
        _cstringInStr = PYTHON2_CSTRING_IN_STR;
        if (!CheckDictAndStrOffsets(dictForTypeType)) {
          std::cerr << "Warning: Failed to confirm dict and str offsets for "
                       "python2.\n";
          succeeded = false;
        }
        break;
      case Version3:
        _keysInDict = PYTHON3_KEYS_IN_DICT;
        _triplesInDictKeys = PYTHON3_TRIPLES_IN_DICT_KEYS;
        _cstringInStr = PYTHON3_CSTRING_IN_STR;
        if (!CheckDictAndStrOffsets(dictForTypeType)) {
          std::cerr << "Warning: Failed to confirm dict and str offsets for "
                       "python3.\n";
          succeeded = false;
        }
        break;
      case VersionUnknownOrOther:
        _keysInDict = PYTHON2_KEYS_IN_DICT;
        _triplesInDictKeys = PYTHON2_TRIPLES_IN_DICT_KEYS;
        _cstringInStr = PYTHON2_CSTRING_IN_STR;
        if (!CheckDictAndStrOffsets(dictForTypeType)) {
          _keysInDict = PYTHON3_KEYS_IN_DICT;
          _triplesInDictKeys = PYTHON3_TRIPLES_IN_DICT_KEYS;
          _cstringInStr = PYTHON3_CSTRING_IN_STR;
          if (!CheckDictAndStrOffsets(dictForTypeType)) {
            std::cerr << "Warning: Failed to determine offsets for python dict "
                         "and str.\n";
            succeeded = false;
          }
        }
        break;
    }
    return succeeded;
  }

  /*
   * Check that the calculated offsets for str work, given that the dict for
   * the type type always contains an str  key "__base__".  If a matching
   * str is found, use this to register the type object for str.
   */
  bool CheckDictAndStrOffsets(Offset dictForTypeType) {
    Reader reader(_virtualAddressMap);
    Offset dictKeys = reader.ReadOffset(dictForTypeType + _keysInDict, 0xbad);
    if ((dictKeys & (sizeof(Offset) - 1)) != 0) {
      return false;
    }
    Offset capacity = 0;
    /*
     * Warning: This is not really sufficiently general but happens to work for
     * the dictionary associated with the python type type.
     */
    if (_triplesInDictKeys > 0) {
      capacity =
          reader.ReadOffset(dictKeys + PYTHON3_CAPACITY_IN_DICT_KEYS, ~0);
      if (capacity == (Offset)(~0)) {
        return false;
      }
    } else {
      Offset mask =
          reader.ReadOffset(dictForTypeType + PYTHON2_MASK_IN_DICT, ~0);
      if (mask == (Offset)(~0)) {
        return false;
      }
      capacity = mask + 1;
    }
    Offset triples = dictKeys + _triplesInDictKeys;
    Offset triplesLimit = triples + capacity * 3 * sizeof(Offset);
    for (Offset triple = triples; triple < triplesLimit;
         triple += 3 * sizeof(Offset)) {
      if (reader.ReadOffset(triple, 0) == 0) {
        continue;
      }
      if (reader.ReadOffset(triple + 2 * sizeof(Offset), 0) == 0) {
        continue;
      }
      Offset strCandidate = reader.ReadOffset(triple + sizeof(Offset), 0);
      if (strCandidate == 0) {
        continue;
      }
      const char* strImage;
      Offset numStrBytesFound =
          _virtualAddressMap.FindMappedMemoryImage(strCandidate, &strImage);
      if (numStrBytesFound < _cstringInStr + 2) {
        continue;
      }
      Offset length = *((Offset*)(strImage + LENGTH_IN_STR));
      if (length != 8) {
        continue;
      }
      if (numStrBytesFound < _cstringInStr + length + 1) {
        continue;
      }
      if (*(strImage + _cstringInStr + length) != 0) {
        continue;
      }
      if (!strcmp("__base__", strImage + _cstringInStr)) {
        _strType = *((Offset*)(strImage + TYPE_IN_PYOBJECT));
        _typeDirectory.RegisterType(_strType, "str");
        return true;
      }
    }
    return false;
  }

  void FindNonEmptyGarbageCollectionLists(Offset base, Offset limit,
                                          Reader& reader) {
    if (_majorVersion == Version2) {
      _garbageCollectionHeaderSize = PYTHON2_GARBAGE_COLLECTION_HEADER_SIZE;
    } else if (_majorVersion == Version3) {
      _garbageCollectionHeaderSize = PYTHON3_GARBAGE_COLLECTION_HEADER_SIZE;
    }

    Reader otherReader(_virtualAddressMap);
    Offset listCandidateLimit = limit - 2 * sizeof(Offset);
    for (Offset listCandidate = base; listCandidate < listCandidateLimit;
         listCandidate += sizeof(Offset)) {
      Offset firstEntry = reader.ReadOffset(listCandidate, 0);
      if (firstEntry == 0 || firstEntry == listCandidate) {
        continue;
      }
      if (otherReader.ReadOffset(firstEntry + sizeof(Offset), 0) !=
          listCandidate) {
        continue;
      }
      Offset lastEntry = reader.ReadOffset(listCandidate + sizeof(Offset), 0);
      if (lastEntry == 0 || lastEntry == listCandidate) {
        continue;
      }
      if (otherReader.ReadOffset(lastEntry, 0) != listCandidate) {
        continue;
      }
      bool foundList = false;
      if (_garbageCollectionHeaderSize == 0) {
        Offset objectType = otherReader.ReadOffset(
            firstEntry + PYTHON2_GARBAGE_COLLECTION_HEADER_SIZE +
                TYPE_IN_PYOBJECT,
            0);
        if (objectType != 0 &&
            otherReader.ReadOffset(objectType + TYPE_IN_PYOBJECT, 0) ==
                _typeType) {
          foundList = true;
          _garbageCollectionHeaderSize = PYTHON2_GARBAGE_COLLECTION_HEADER_SIZE;
        }
        if (!foundList) {
          objectType = otherReader.ReadOffset(
              firstEntry + PYTHON3_GARBAGE_COLLECTION_HEADER_SIZE +
                  TYPE_IN_PYOBJECT,
              0);
          if (objectType != 0 &&
              otherReader.ReadOffset(objectType + TYPE_IN_PYOBJECT, 0) ==
                  _typeType) {
            foundList = true;
            _garbageCollectionHeaderSize =
                PYTHON3_GARBAGE_COLLECTION_HEADER_SIZE;
          }
        }
      } else {
        Offset objectType = otherReader.ReadOffset(
            firstEntry + _garbageCollectionHeaderSize + TYPE_IN_PYOBJECT, 0);
        foundList = (objectType != 0 &&
                     otherReader.ReadOffset(objectType + TYPE_IN_PYOBJECT, 0) ==
                         _typeType);
      }
      if (foundList) {
        _nonEmptyGarbageCollectionLists.push_back(listCandidate);
        listCandidate += 2 * sizeof(Offset);
      }
    }
  }
};
}  // namespace Python
}  // namespace chap
