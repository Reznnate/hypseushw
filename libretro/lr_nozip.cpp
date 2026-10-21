/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 HypseusHW contributors
 *
 * libzippp without libzip: zip-packed Singe games (-zlua) are not supported
 * in the libretro core, so every archive fails to open.
 */
#include "io/zippp.h"

using namespace libzippp;

ZipArchive::ZipArchive(const std::string &zipPath, const std::string &pw, Encryption)
    : path(zipPath), zipHandle(nullptr), zipSource(nullptr), mode(NotOpen), password(pw) {}
ZipArchive::~ZipArchive(void) {}
ZipEntry::ZipEntry(void) : zipFile(nullptr), index(0), time(0), compressionMethod(0), encryptionMethod(0), size(0), sizeComp(0), crc(0) {}
bool ZipArchive::open(OpenMode, bool) { return false; }
int ZipArchive::close(void) { return LIBZIPPP_OK; }
std::vector<ZipEntry> ZipArchive::getEntries(State) const { return std::vector<ZipEntry>(); }
ZipEntry ZipArchive::getEntry(const std::string &, bool, bool, State) const { return ZipEntry(); }
ZipEntry ZipArchive::getEntry(libzippp_int64, State) const { return ZipEntry(); }
void *ZipEntry::readAsBinary(ZipArchive::State, libzippp_uint64) const { return nullptr; }
