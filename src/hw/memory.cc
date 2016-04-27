#include "core/math.h"
#include "core/memory.h"
#include "hw/machine.h"
#include "hw/memory.h"

using namespace re;
using namespace re::hw;
using namespace re::sys;

static bool PageAligned(uint32_t start, uint32_t size) {
  return (start & (PAGE_OFFSET_BITS - 1)) == 0 &&
         ((start + size) & (PAGE_OFFSET_BITS - 1)) == 0;
}

// map virtual addresses to pages
static int PageIndex(uint32_t addr) { return addr >> PAGE_OFFSET_BITS; }

static uint32_t PageOffset(uint32_t addr) { return addr & PAGE_OFFSET_MASK; }

// pack and unpack page entry bitstrings
static PageEntry PackEntry(const MemoryRegion &region, uint32_t region_offset) {
  return region_offset | (region.dynamic ? 0 : REGION_TYPE_MASK) |
         region.handle;
}

static uint32_t RegionOffset(const PageEntry &page) {
  return page & REGION_OFFSET_MASK;
}

static int RegionTypeIsStatic(const PageEntry &page) {
  return page & REGION_TYPE_MASK;
}

static int RegionIndex(const PageEntry &page) {
  return page & REGION_INDEX_MASK;
}

// iterate mirrors for a given address and mask
struct MirrorIterator {
  MirrorIterator(uint32_t addr, uint32_t mask)
      : base(addr & mask),
        mask(mask),
        imask(~mask),
        step(1 << ctz(imask)),
        i(0),
        addr(base),
        first(true) {}

  uint32_t base, mask, imask, step;
  uint32_t i, addr;
  bool first;
};

static bool NextMirror(MirrorIterator *it) {
  // first iteration just returns base
  if (it->first) {
    it->first = false;
    return true;
  }

  // stop once mask complement is completely set
  if ((it->addr & it->imask) == it->imask) {
    return false;
  }

  // step to the next permutation
  it->i += it->step;

  // if the new value carries over into a masked off bit, skip it
  uint32_t carry;
  do {
    carry = it->i & it->mask;
    it->i += carry;
  } while (carry);

  // merge with the base
  it->addr = it->base | it->i;

  return true;
}

static bool ReserveAddressSpace(uint8_t **base) {
  // find a contiguous (1 << 32) byte chunk of memory to map an address space to
  int i = 64;

  while (i > 32) {
    i--;

    *base = reinterpret_cast<uint8_t *>(1ull << i);

    if (!ReservePages(*base, ADDRESS_SPACE_SIZE)) {
      continue;
    }

    // reservation was a success, release now so shared memory can be mapped
    // into it
    ReleasePages(*base, ADDRESS_SPACE_SIZE);

    return true;
  }

  LOG_WARNING("Failed to reserve address space");

  return false;
}

Memory::Memory(Machine &machine)
    : machine_(machine), shmem_(SHMEM_INVALID), shmem_size_(0) {
  // 0 page is reserved, meaning all valid page entries must be non-zero
  num_regions_ = 1;
}

Memory::~Memory() { DestroySharedMemory(); }

bool Memory::Init() {
  if (!CreateSharedMemory()) {
    return false;
  }

  // map each memory interface's address space
  for (auto device : machine_.devices()) {
    MemoryInterface *memory = device->memory();

    if (!memory) {
      continue;
    }

    // create the actual address map
    AddressMap map;
    memory->mapper()(device, machine_, map);

    // apply the map to create the address space
    CHECK(memory->space().Map(map));
  }

  return true;
}

bool Memory::CreateSharedMemory() {
  // create the shared memory object to back the address space
  shmem_ = ::CreateSharedMemory("/redream", ADDRESS_SPACE_SIZE, ACC_READWRITE);

  if (shmem_ == SHMEM_INVALID) {
    LOG_WARNING("Failed to create shared memory object");
    return false;
  }

  return true;
}

void Memory::DestroySharedMemory() { ::DestroySharedMemory(shmem_); }

MemoryRegion *Memory::CreateRegion(uint32_t size) {
  CHECK(PageAligned(shmem_size_, size));

  MemoryRegion *region = AllocRegion(size);
  region->dynamic = false;

  return region;
}

MemoryRegion *Memory::CreateRegion(uint32_t size, R8Delegate r8,
                                   R16Delegate r16, R32Delegate r32,
                                   R64Delegate r64, W8Delegate w8,
                                   W16Delegate w16, W32Delegate w32,
                                   W64Delegate w64) {
  MemoryRegion *region = AllocRegion(size);

  region->dynamic = true;
  region->r8 = r8;
  region->r16 = r16;
  region->r32 = r32;
  region->r64 = r64;
  region->w8 = w8;
  region->w16 = w16;
  region->w32 = w32;
  region->w64 = w64;

  return region;
}

MemoryRegion *Memory::AllocRegion(uint32_t size) {
  CHECK_LT(num_regions_, MAX_REGIONS);
  CHECK(PageAligned(shmem_size_, size));

  MemoryRegion *region = &regions_[num_regions_];
  new (region) MemoryRegion();
  region->handle = num_regions_++;
  region->shmem_offset = shmem_size_;
  region->size = size;

  shmem_size_ += size;

  return region;
}

AddressMap::AddressMap() : entries_(), num_entries_(0) {}

void AddressMap::MountRegion(MemoryRegion *region, uint32_t size, uint32_t addr,
                             uint32_t addr_mask) {
  MapEntry &entry = AllocEntry();
  entry.type = MAP_ENTRY_MOUNT;
  entry.size = size;
  entry.addr = addr;
  entry.addr_mask = addr_mask;
  entry.mount.region = region;
}

void AddressMap::MountDevice(Device *device, AddressMapper mapper,
                             uint32_t size, uint32_t addr, uint32_t addr_mask) {
  MapEntry &entry = AllocEntry();
  entry.type = MAP_ENTRY_DEVICE;
  entry.size = size;
  entry.addr = addr;
  entry.addr_mask = addr_mask;
  entry.device.device = device;
  entry.device.mapper = mapper;
}

void AddressMap::Mirror(uint32_t physical_addr, uint32_t size, uint32_t addr) {
  MapEntry &entry = AllocEntry();
  entry.type = MAP_ENTRY_MIRROR;
  entry.size = size;
  entry.addr = addr;
  entry.addr_mask = 0xffffffff;
  entry.mirror.physical_addr = physical_addr;
}

MapEntry &AddressMap::AllocEntry() {
  CHECK_LT(num_entries_, MAX_REGIONS);
  MapEntry &entry = entries_[num_entries_++];
  new (&entry) MapEntry();
  return entry;
}

uint8_t AddressSpace::R8(void *space, uint32_t addr) {
  return reinterpret_cast<AddressSpace *>(space)->R8(addr);
}
uint16_t AddressSpace::R16(void *space, uint32_t addr) {
  return reinterpret_cast<AddressSpace *>(space)->R16(addr);
}
uint32_t AddressSpace::R32(void *space, uint32_t addr) {
  return reinterpret_cast<AddressSpace *>(space)->R32(addr);
}
uint64_t AddressSpace::R64(void *space, uint32_t addr) {
  return reinterpret_cast<AddressSpace *>(space)->R64(addr);
}
void AddressSpace::W8(void *space, uint32_t addr, uint8_t value) {
  reinterpret_cast<AddressSpace *>(space)->W8(addr, value);
}
void AddressSpace::W16(void *space, uint32_t addr, uint16_t value) {
  reinterpret_cast<AddressSpace *>(space)->W16(addr, value);
}
void AddressSpace::W32(void *space, uint32_t addr, uint32_t value) {
  reinterpret_cast<AddressSpace *>(space)->W32(addr, value);
}
void AddressSpace::W64(void *space, uint32_t addr, uint64_t value) {
  reinterpret_cast<AddressSpace *>(space)->W64(addr, value);
}

AddressSpace::AddressSpace(Machine &machine)
    : machine_(machine),
      memory_(*machine_.memory()),
      base_(nullptr),
      protected_base_(nullptr) {}

AddressSpace::~AddressSpace() { Unmap(); }

bool AddressSpace::Map(const AddressMap &map) {
  Unmap();

  // flatten the supplied address map out into a virtual page table
  MergeToPageTable(map, 0);

  // map the virtual page table into both the base and protected mirrors
  if (!ReserveAddressSpace(&base_) || !MapPageTable(base_)) {
    return false;
  }

  if (!ReserveAddressSpace(&protected_base_) ||
      !MapPageTable(protected_base_)) {
    return false;
  }

  // protect dynamic regions in the protected address space
  for (int page_index = 0; page_index < NUM_PAGES; page_index++) {
    PageEntry page = pages_[page_index];

    if (RegionTypeIsStatic(page)) {
      continue;
    }

    uint32_t addr = page_index * PAGE_BLKSIZE;
    ProtectPages(protected_base_ + addr, PAGE_BLKSIZE, ACC_NONE);
  }

  return true;
}

void AddressSpace::Unmap() {
  UnmapPageTable(base_);
  UnmapPageTable(protected_base_);
}

uint8_t *AddressSpace::Translate(uint32_t addr) { return base_ + addr; }

uint8_t *AddressSpace::TranslateProtected(uint32_t addr) {
  return protected_base_ + addr;
}

uint8_t AddressSpace::R8(uint32_t addr) {
  return ReadBytes<uint8_t, &MemoryRegion::r8>(addr);
}

uint16_t AddressSpace::R16(uint32_t addr) {
  return ReadBytes<uint16_t, &MemoryRegion::r16>(addr);
}

uint32_t AddressSpace::R32(uint32_t addr) {
  return ReadBytes<uint32_t, &MemoryRegion::r32>(addr);
}

uint64_t AddressSpace::R64(uint32_t addr) {
  return ReadBytes<uint64_t, &MemoryRegion::r64>(addr);
}

void AddressSpace::W8(uint32_t addr, uint8_t value) {
  WriteBytes<uint8_t, &MemoryRegion::w8>(addr, value);
}

void AddressSpace::W16(uint32_t addr, uint16_t value) {
  WriteBytes<uint16_t, &MemoryRegion::w16>(addr, value);
}

void AddressSpace::W32(uint32_t addr, uint32_t value) {
  WriteBytes<uint32_t, &MemoryRegion::w32>(addr, value);
}

void AddressSpace::W64(uint32_t addr, uint64_t value) {
  WriteBytes<uint64_t, &MemoryRegion::w64>(addr, value);
}

void AddressSpace::Memcpy(uint32_t dst, const void *ptr, uint32_t size) {
  CHECK(size % 4 == 0);

  const uint8_t *src = reinterpret_cast<const uint8_t *>(ptr);
  uint32_t end = dst + size;
  while (dst < end) {
    W32(dst, re::load<uint32_t>(src));
    dst += 4;
    src += 4;
  }
}

void AddressSpace::Memcpy(void *ptr, uint32_t src, uint32_t size) {
  CHECK(size % 4 == 0);

  uint8_t *dst = reinterpret_cast<uint8_t *>(ptr);
  uint8_t *end = dst + size;
  while (dst < end) {
    re::store(dst, R32(src));
    src += 4;
    dst += 4;
  }
}

void AddressSpace::Memcpy(uint32_t dst, uint32_t src, uint32_t size) {
  CHECK(size % 4 == 0);

  uint32_t end = dst + size;
  while (dst < end) {
    W32(dst, R32(src));
    src += 4;
    dst += 4;
  }
}

void AddressSpace::Lookup(uint32_t addr, uint8_t **ptr, MemoryRegion **region,
                          uint32_t *offset) {
  PageEntry page = pages_[PageIndex(addr)];

  if (RegionTypeIsStatic(page)) {
    *ptr = base_ + addr;
  } else {
    *ptr = nullptr;
  }

  *region = &memory_.regions_[RegionIndex(page)];
  *offset = RegionOffset(page) + PageOffset(addr);
}

void AddressSpace::MergeToPageTable(const AddressMap &map, uint32_t offset) {
  // iterate regions in the supplied memory map in the other added, flattening
  // them out into a virtual page table
  for (int i = 0, n = map.num_entries(); i < n; i++) {
    MapEntry *entry = const_cast<MapEntry *>(map.entry(i));

    // iterate each mirror of the entry
    MirrorIterator it(offset + entry->addr, entry->addr_mask);

    while (NextMirror(&it)) {
      uint32_t addr = it.addr;
      uint32_t size = entry->size;
      CHECK(PageAligned(addr, size));

      int first_page = PageIndex(addr);
      int num_pages = size >> PAGE_OFFSET_BITS;

      switch (entry->type) {
        case MAP_ENTRY_MOUNT: {
          MemoryRegion *region = entry->mount.region;

          // create an entry in the page table for each page the region occupies
          for (int i = 0; i < num_pages; i++) {
            uint32_t region_offset = i * PAGE_BLKSIZE;

            pages_[first_page + i] = PackEntry(*region, region_offset);
          }
        } break;

        case MAP_ENTRY_DEVICE: {
          AddressMap device_map;
          entry->device.mapper(entry->device.device, machine_, device_map);
          MergeToPageTable(device_map, addr);
        } break;

        case MAP_ENTRY_MIRROR: {
          CHECK(PageAligned(entry->mirror.physical_addr, size));

          int first_physical_page = PageIndex(entry->mirror.physical_addr);

          // copy the page entries for the requested physical range into the new
          // virtual address range
          for (int i = 0; i < num_pages; i++) {
            pages_[first_page + i] = pages_[first_physical_page + i];
          }
        } break;
      }
    }
  }
}

uint32_t AddressSpace::GetPageOffset(const PageEntry &page) const {
  const MemoryRegion &region = memory_.regions_[RegionIndex(page)];
  return region.shmem_offset + RegionOffset(page);
}

int AddressSpace::GetNumAdjacentPages(int first_page_index) const {
  int i;

  for (i = first_page_index; i < NUM_PAGES - 1; i++) {
    PageEntry page = pages_[i];
    PageEntry next_page = pages_[i + 1];

    uint32_t page_offset = GetPageOffset(page);
    uint32_t next_page_offset = GetPageOffset(next_page);

    if ((next_page_offset - page_offset) != PAGE_BLKSIZE) {
      break;
    }
  }

  return (i + 1) - first_page_index;
}

bool AddressSpace::MapPageTable(uint8_t *base) {
  for (int page_index = 0; page_index < NUM_PAGES;) {
    PageEntry page = pages_[page_index];

    if (!page) {
      page_index++;
      continue;
    }

    // batch map djacent pages, mmap is fairly slow
    int num_pages = GetNumAdjacentPages(page_index);
    uint32_t size = num_pages * PAGE_BLKSIZE;

    // mmap the virtual address range to the raw address space
    uint32_t addr = page_index * PAGE_BLKSIZE;
    uint32_t page_offset = GetPageOffset(page);

    if (!MapSharedMemory(memory_.shmem_, page_offset, base + addr, size,
                         ACC_READWRITE)) {
      return false;
    }

    page_index += num_pages;
  }

  return true;
}

void AddressSpace::UnmapPageTable(uint8_t *base) {
  if (!base) {
    return;
  }

  for (int page_index = 0; page_index < NUM_PAGES;) {
    PageEntry page = pages_[page_index];

    if (!page) {
      page_index++;
      continue;
    }

    uint32_t addr = page_index * PAGE_BLKSIZE;

    int num_pages = GetNumAdjacentPages(page_index);
    uint32_t size = num_pages * PAGE_BLKSIZE;

    CHECK(UnmapSharedMemory(memory_.shmem_, base + addr, size));

    page_index += num_pages;
  }
}

template <typename INT, delegate<INT(uint32_t)> MemoryRegion::*DELEGATE>
inline INT AddressSpace::ReadBytes(uint32_t addr) {
  PageEntry page = pages_[PageIndex(addr)];
  DCHECK(page);

  if (RegionTypeIsStatic(page)) {
    return re::load<INT>(base_ + addr);
  }

  MemoryRegion &region = memory_.regions_[RegionIndex(page)];
  uint32_t region_offset = RegionOffset(page);
  uint32_t page_offset = PageOffset(addr);
  return (region.*DELEGATE)(region_offset + page_offset);
}

template <typename INT, delegate<void(uint32_t, INT)> MemoryRegion::*DELEGATE>
inline void AddressSpace::WriteBytes(uint32_t addr, INT value) {
  PageEntry page = pages_[PageIndex(addr)];
  DCHECK(page);

  if (RegionTypeIsStatic(page)) {
    re::store(base_ + addr, value);
    return;
  }

  MemoryRegion &region = memory_.regions_[RegionIndex(page)];
  uint32_t region_offset = RegionOffset(page);
  uint32_t page_offset = PageOffset(addr);
  (region.*DELEGATE)(region_offset + page_offset, value);
}
