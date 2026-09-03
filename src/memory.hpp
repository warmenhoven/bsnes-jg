/*
 * bsnes-jg - Super Nintendo emulator
 *
 * Copyright (C) 2004-2020 byuu
 * Copyright (C) 2020-2022 Rupert Carmichael
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, specifically version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace SuperFamicom {

struct Memory {
  static bool GlobalWriteEnable;

  virtual ~Memory();
  inline explicit operator bool() const;

  virtual void reset() {}
  virtual void allocate(unsigned, uint8_t = 0xff) {}

  virtual uint8_t* data() = 0;
  virtual unsigned size() const = 0;

  virtual uint8_t read(unsigned, uint8_t = 0) = 0;
  virtual void write(unsigned, uint8_t) = 0;

  unsigned id = 0;
};

struct ReadableMemory : Memory {
  inline void reset() override;
  inline void allocate(unsigned, uint8_t = 0xff) override;
  inline uint8_t* data() override;
  inline unsigned size() const override;
  inline uint8_t read(unsigned, uint8_t = 0) override;
  inline void write(unsigned, uint8_t) override;
  inline uint8_t operator[](unsigned) const;

private:
  struct {
    uint8_t* data = nullptr;
    unsigned size = 0;
  } self;
};

struct WritableMemory : Memory {
  inline void reset() override;
  inline void allocate(unsigned, uint8_t = 0xff) override;
  inline uint8_t* data() override;
  inline unsigned size() const override;
  inline uint8_t read(unsigned, uint8_t = 0) override;
  inline void write(unsigned, uint8_t) override;
  inline uint8_t& operator[](unsigned);

private:
  struct {
    uint8_t* data = nullptr;
    unsigned size = 0;
  } self;
};

struct Bus {
  inline static unsigned mirror(unsigned, unsigned);
  inline static unsigned reduce(unsigned, unsigned);

  ~Bus();

  inline uint8_t read(unsigned, uint8_t = 0);
  inline void write(unsigned, uint8_t);

  void reset();
  unsigned map(
    const std::function<uint8_t (unsigned, uint8_t)>&,
    const std::function<void (unsigned, uint8_t)>&,
    const std::string&, unsigned = 0, unsigned = 0, unsigned = 0,
    uint8_t* = nullptr, unsigned = 0, bool = false
  );
  void unmap(const std::string&);

private:
  //Direct-pointer fast path. A page qualifies only when a single mapping covers
  //it end to end and target[] runs linearly across it; map() detects that as it
  //walks the range, so there is no separate analysis pass. Every other page
  //keeps the function path, which is what preserves MMIO side effects, open bus
  //and the Memory::GlobalWriteEnable gate on ROM writes.
  enum : unsigned {
    fastPageBits = 13,                       //8 KiB: the smallest window any
    fastPageSize = 1u << fastPageBits,       //mapping uses, so the WRAM mirrors
    fastPageMask = fastPageSize - 1,         //still qualify
    fastPageCount = 0x1000000 >> fastPageBits
  };

  //points at the backing byte for offset 0 of the page, so indexing with
  //(addr & fastPageMask) always stays inside the allocation
  uint8_t* fastRead[fastPageCount] = {};
  uint8_t* fastWrite[fastPageCount] = {};

  uint8_t *lookup = nullptr;
  uint32_t *target = nullptr;

  std::function<uint8_t (unsigned, uint8_t)> reader[256];
  std::function<void  (unsigned, uint8_t)> writer[256];
  unsigned counter[256];
};

extern Bus bus;

Memory::operator bool() const {
  return size() > 0;
}

void ReadableMemory::reset() {
  delete[] self.data;
  self.data = nullptr;
  self.size = 0;
}

void ReadableMemory::allocate(unsigned size, uint8_t fill) {
  if(self.size != size) {
    delete[] self.data;
    self.data = new uint8_t[self.size = size];
  }
  for(unsigned address = 0; address < size; ++address) {
    self.data[address] = fill;
  }
}

uint8_t* ReadableMemory::data() {
  return self.data;
}

unsigned ReadableMemory::size() const {
  return self.size;
}

uint8_t ReadableMemory::read(unsigned address, uint8_t) {
  return self.data[address];
}

void ReadableMemory::write(unsigned address, uint8_t data) {
  if(Memory::GlobalWriteEnable) {
    self.data[address] = data;
  }
}

uint8_t ReadableMemory::operator[](unsigned address) const {
  return self.data[address];
}

void WritableMemory::reset() {
  delete[] self.data;
  self.data = nullptr;
  self.size = 0;
}

void WritableMemory::allocate(unsigned size, uint8_t fill) {
  if(self.size != size) {
    delete[] self.data;
    self.data = new uint8_t[self.size = size];
  }
  for(unsigned address = 0; address < size; ++address) {
    self.data[address] = fill;
  }
}

uint8_t* WritableMemory::data() {
  return self.data;
}

unsigned WritableMemory::size() const {
  return self.size;
}

uint8_t WritableMemory::read(unsigned address, uint8_t) {
  return self.data[address];
}

void WritableMemory::write(unsigned address, uint8_t data)  {
  self.data[address] = data;
}

uint8_t& WritableMemory::operator[](unsigned address) {
  return self.data[address];
}

unsigned Bus::mirror(unsigned addr, unsigned size) {
  if(size == 0) return 0;
  unsigned base = 0;
  unsigned mask = 1 << 23;
  while(addr >= size) {
    while(!(addr & mask)) mask >>= 1;
    addr -= mask;
    if(size > mask) {
      size -= mask;
      base += mask;
    }
    mask >>= 1;
  }
  return base + addr;
}

unsigned Bus::reduce(unsigned addr, unsigned mask) {
  while(mask) {
    unsigned bits = (mask & -mask) - 1;
    addr = ((addr >> 1) & ~bits) | (addr & bits);
    mask = (mask & (mask - 1)) >> 1;
  }
  return addr;
}

uint8_t Bus::read(unsigned addr, uint8_t data) {
  if(uint8_t* page = fastRead[addr >> fastPageBits]) return page[addr & fastPageMask];
  return reader[lookup[addr]](target[addr], data);
}

void Bus::write(unsigned addr, uint8_t data) {
  if(uint8_t* page = fastWrite[addr >> fastPageBits]) {
    page[addr & fastPageMask] = data;
    return;
  }
  return writer[lookup[addr]](target[addr], data);
}

}
