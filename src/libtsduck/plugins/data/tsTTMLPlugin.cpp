//----------------------------------------------------------------------------
//
// TSDuck - The MPEG Transport Stream Toolkit
// Copyright (c) 2005-2026, Thierry Lelegard
// BSD-2-Clause license, see LICENSE.txt file or https://tsduck.io/license
//
//----------------------------------------------------------------------------

#include "tsTTMLPlugin.h"
#include "tsPluginRepository.h"
#include "tsTTMLSubtitlingDescriptor.h"
#include "tsCRC32.h"

TS_REGISTER_PROCESSOR_PLUGIN(u"ttml", ts::TTMLPlugin);

namespace {
    // PES data field of a DVB-TTML stream, EN 303 560 clause 5.2.2.2, table 16.
    constexpr size_t MEDIATIME_SIZE = 6;         // segment_mediatime, units of 100 µs
    constexpr size_t SEGMENT_HEADER_SIZE = 3;    // segment_type, segment_length
    constexpr size_t CRC_SIZE = 4;
    constexpr uint8_t SEGMENT_TYPE_TTML = 0x01;
    constexpr uint8_t SEGMENT_TYPE_GZIP_TTML = 0x02;
    constexpr uint64_t MEDIATIME_UNITS_PER_SECOND = 10'000;

    // Read the PTS in the header of a PES packet, if it has one.
    uint64_t PESHeaderPTS(const ts::PESPacket& packet)
    {
        const uint8_t* header = packet.header();
        const size_t header_size = packet.headerSize();
        const bool has_pts = packet.hasLongHeader() && header_size >= 14 && (header[7] & 0x80) != 0;
        if (!has_pts) {
            return ts::INVALID_PTS;
        }
        return (uint64_t(header[9] & 0x0E) << 29) |
               (uint64_t(header[10]) << 22) |
               (uint64_t(header[11] & 0xFE) << 14) |
               (uint64_t(header[12]) << 7) |
               (uint64_t(header[13]) >> 1);
    }
}


//----------------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------------

ts::TTMLPlugin::TTMLPlugin(TSP* tsp_) :
    ProcessorPlugin(tsp_, u"Extract DVB-TTML subtitles", u"[options]")
{
    duck.defineArgsForCharset(*this);

    option(u"language", 'l', STRING);
    help(u"language", u"name",
         u"Select the DVB-TTML PIDs whose TTML_subtitling_descriptor has this language. "
         u"By default, all DVB-TTML PIDs of the service are selected.");

    option(u"max-documents", 'm', POSITIVE);
    help(u"max-documents",
         u"Stop after extracting this number of TTML documents, across all selected PIDs. "
         u"By default, all documents are extracted.");

    option(u"no-duplicate");
    help(u"no-duplicate",
         u"Do not report a PES packet whose data field is identical to the previous one in the same PID. "
         u"A DVB-TTML stream usually repeats each document in several PES packets. "
         u"With this option, a repeated document is not listed, not saved, and does not count towards --max-documents.");

    option(u"output-directory", 'd', DIRECTORY);
    help(u"output-directory",
         u"Save each TTML document in a file in this directory. "
         u"The file name is ttml-<pid>-<index>.xml, or .xml.gz for a gzip-compressed segment. "
         u"By default, the documents are not saved.");

    option(u"output-file", 'o', FILENAME);
    help(u"output-file", u"filename",
         u"Write the description of each PES packet to this text file. "
         u"By default, it goes to the standard output.");

    option(u"pid", 'p', PIDVAL, 0, UNLIMITED_COUNT);
    help(u"pid", u"pid1[-pid2]",
         u"Extract DVB-TTML from these PIDs. Several --pid options may be specified. "
         u"By default, the PIDs come from the TTML_subtitling_descriptors in the PMT of the service.");

    option(u"service", 's', STRING);
    help(u"service",
         u"Specifies the service with DVB-TTML subtitles. If the argument is an "
         u"integer value (either decimal or hexadecimal), it is interpreted as a "
         u"service id. Otherwise, it is interpreted as a service name, as specified "
         u"in the SDT. The name is not case sensitive and blanks are ignored. "
         u"If neither --service nor --pid is specified, the first service in the PAT is used.");
}


//----------------------------------------------------------------------------
// Start and stop methods
//----------------------------------------------------------------------------

bool ts::TTMLPlugin::start()
{
    duck.loadArgs(*this);
    _service.set(value(u"service"));
    getIntValues(_pids, u"pid");
    getIntValue(_max_documents, u"max-documents", 0);
    _no_duplicate = present(u"no-duplicate");
    getValue(_language, u"language");
    getPathValue(_out_file, u"output-file");
    getPathValue(_out_dir, u"output-directory");

    _out = &std::cout;
    if (!_out_file.empty()) {
        _out_stream.open(_out_file);
        if (!_out_stream) {
            error(u"cannot create %s", _out_file);
            return false;
        }
        _out = &_out_stream;
    }

    _abort = false;
    _document_count = 0;
    _counts.clear();
    _last_data.clear();
    _demux.reset();
    _pids_known = _pids.any();
    _demux.setPIDFilter(_pids);
    return true;
}

bool ts::TTMLPlugin::stop()
{
    if (_out_stream.is_open()) {
        _out_stream.close();
    }
    return true;
}


//----------------------------------------------------------------------------
// Find the DVB-TTML PIDs in the PMT of the service.
//----------------------------------------------------------------------------

void ts::TTMLPlugin::handlePMT(const PMT& pmt, PID)
{
    const EDID ttml_edid(EDID::ExtensionDVB(XDID_DVB_TTML_SUBTITLING));

    for (const auto& [pid, stream] : pmt.streams) {
        for (size_t index = stream.descs.search(ttml_edid); index < stream.descs.count(); index = stream.descs.search(ttml_edid, index + 1)) {
            const TTMLSubtitlingDescriptor desc(duck, stream.descs[index]);
            const bool language_matches = _language.empty() || (desc.isValid() && _language.similar(desc.language_code));
            if (language_matches) {
                _pids.set(pid);
                verbose(u"using DVB-TTML PID %n, language \"%s\", purpose: %s", pid, desc.language_code, TTMLSubtitlingDescriptor::TTML_subtitle_purpose(desc.subtitle_purpose));
            }
        }
    }

    if (_pids.none()) {
        if (_language.empty()) {
            error(u"no DVB-TTML subtitles found for service %n", pmt.service_id);
        }
        else {
            error(u"no DVB-TTML subtitles found for language \"%s\" in service %n", _language, pmt.service_id);
        }
        _abort = true;
        return;
    }

    _pids_known = true;
    _demux.setPIDFilter(_pids);
}


//----------------------------------------------------------------------------
// Invoked for each complete PES packet on a DVB-TTML PID.
//----------------------------------------------------------------------------

void ts::TTMLPlugin::handlePESPacket(PESDemux&, const PESPacket& packet)
{
    const PID pid = packet.sourcePID();
    const size_t pes_index = ++_counts[pid];
    const uint8_t* data = packet.payload();
    const size_t size = packet.payloadSize();
    const uint64_t pts = PESHeaderPTS(packet);

    UString line(UString::Format(u"PID %n, PES %d", pid, pes_index));
    if (pts != INVALID_PTS) {
        line += UString::Format(u", PTS 0x%09X (%.3f s)", pts, double(pts) / double(SYSTEM_CLOCK_SUBFREQ));
    }

    if (size < MEDIATIME_SIZE + 1 + CRC_SIZE) {
        line += UString::Format(u", data field of %d bytes is too short", size);
        *_out << line << std::endl;
        return;
    }

    const size_t body_size = size - CRC_SIZE;
    const uint32_t expected_crc = GetUInt32(data + body_size);
    const uint32_t actual_crc = CRC32(data, body_size).value();
    if (expected_crc != actual_crc) {
        warning(u"PID %n, PES %d: CRC_32 is 0x%08X, the data field computes to 0x%08X", pid, pes_index, expected_crc, actual_crc);
        line += UString::Format(u", CRC error");
        *_out << line << std::endl;
        return;
    }

    ByteBlock& last_data(_last_data[pid]);
    const bool is_duplicate = last_data.size() == size && MemEqual(last_data.data(), data, size);
    if (is_duplicate && _no_duplicate) {
        return;
    }
    last_data.copy(data, size);

    const uint64_t mediatime = GetUInt48(data);
    const double mediatime_seconds = double(mediatime) / double(MEDIATIME_UNITS_PER_SECOND);
    line += UString::Format(u", mediatime %.3f s", mediatime_seconds);
    if (pts != INVALID_PTS) {
        // EN 303 560 clause 5.2.4.1: the PTS of the packet corresponds to segment_mediatime on the document timeline.
        const double pts_seconds = double(pts) / double(SYSTEM_CLOCK_SUBFREQ);
        line += UString::Format(u", PTS - mediatime %.3f s", pts_seconds - mediatime_seconds);
    }

    const size_t segment_count = data[MEDIATIME_SIZE];
    size_t position = MEDIATIME_SIZE + 1;
    UStringList segments;
    for (size_t index = 0; index < segment_count; ++index) {
        const size_t segment_data_start = position + SEGMENT_HEADER_SIZE;
        if (segment_data_start > body_size) {
            segments.push_back(u"truncated segment header");
            break;
        }
        const uint8_t segment_type = data[position];
        const size_t segment_length = GetUInt16(data + position + 1);
        const size_t segment_end = segment_data_start + segment_length;
        if (segment_end > body_size) {
            segments.push_back(UString::Format(u"segment of %d bytes runs past the data field", segment_length));
            break;
        }

        const bool is_ttml = segment_type == SEGMENT_TYPE_TTML || segment_type == SEGMENT_TYPE_GZIP_TTML;
        if (segment_type == SEGMENT_TYPE_TTML) {
            segments.push_back(UString::Format(u"TTML %d bytes", segment_length));
        }
        else if (segment_type == SEGMENT_TYPE_GZIP_TTML) {
            segments.push_back(UString::Format(u"gzip TTML %d bytes", segment_length));
        }
        else {
            segments.push_back(UString::Format(u"type 0x%02X %d bytes", segment_type, segment_length));
        }

        if (is_ttml) {
            _document_count++;
            if (!_out_dir.empty()) {
                const UString extension(segment_type == SEGMENT_TYPE_GZIP_TTML ? u".xml.gz" : u".xml");
                const UString name(UString::Format(u"ttml-%04X-%06d%s", pid, pes_index, extension));
                ByteBlock document(data + segment_data_start, segment_length);
                document.saveToFile(UString((_out_dir / name.toUTF8()).u16string()), this);
            }
        }
        position = segment_end;
    }

    line += UString::Format(u", %d segment%s: %s", segment_count, segment_count == 1 ? u"" : u"s", UString::Join(segments));
    *_out << line << std::endl;

    if (_max_documents > 0 && _document_count >= _max_documents) {
        _abort = true;
    }
}


//----------------------------------------------------------------------------
// Packet processing method
//----------------------------------------------------------------------------

ts::PacketProcessStatus ts::TTMLPlugin::processPacket(TSPacket& pkt, TSPacketMetadata& pkt_data)
{
    if (!_pids_known) {
        _service.feedPacket(pkt);
    }
    _demux.feedPacket(pkt);
    return _service.nonExistentService() || _abort ? TSP_END : TSP_OK;
}
