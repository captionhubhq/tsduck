//----------------------------------------------------------------------------
//
// TSDuck - The MPEG Transport Stream Toolkit
// Copyright (c) 2005-2026, Thierry Lelegard
// BSD-2-Clause license, see LICENSE.txt file or https://tsduck.io/license
//
//----------------------------------------------------------------------------
//!
//!  @file
//!  Plugin to extract DVB-TTML subtitles.
//!
//----------------------------------------------------------------------------

#pragma once
#include "tsProcessorPlugin.h"
#include "tsSignalizationHandlerInterface.h"
#include "tsPESHandlerInterface.h"
#include "tsPESDemux.h"
#include "tsServiceDiscovery.h"

namespace ts {
    //!
    //! Plugin to extract DVB-TTML subtitles (ETSI EN 303 560).
    //! @ingroup libtsduck plugin
    //!
    class TSDUCKDLL TTMLPlugin: public ProcessorPlugin, private SignalizationHandlerInterface, private PESHandlerInterface
    {
        TS_PLUGIN_CONSTRUCTORS(TTMLPlugin);
    public:
        // Implementation of plugin API.
        virtual bool start() override;
        virtual bool stop() override;
        virtual PacketProcessStatus processPacket(TSPacket&, TSPacketMetadata&) override;

    private:
        bool             _abort = false;             // Error (service not found, etc).
        PIDSet           _pids {};                   // TTML PIDs to extract.
        bool             _pids_known = false;        // The PIDs come from --pid or have been found in the PMT.
        size_t           _max_documents = 0;         // Max number of documents to extract, zero means no limit.
        size_t           _document_count = 0;        // Number of documents extracted so far.
        bool             _no_duplicate = false;      // Skip a PES packet identical to the previous one in its PID.
        UString          _language {};               // Language to select.
        fs::path         _out_file {};               // Output file name for the log of PES packets.
        fs::path         _out_dir {};                // Output directory for documents.
        std::ofstream    _out_stream {};             // Output file stream, when _out_file is set.
        std::ostream*    _out = &std::cout;          // Where the log of PES packets goes.
        ServiceDiscovery _service {duck, this};      // Service name & id.
        PESDemux         _demux {duck, this, NoPID()};
        std::map<PID, size_t> _counts {};            // Number of PES packets per PID.
        std::map<PID, ByteBlock> _last_data {};      // Data field of the last PES packet per PID.

        // Implementation of interfaces.
        virtual void handlePMT(const PMT&, PID) override;
        virtual void handlePESPacket(PESDemux&, const PESPacket&) override;

    };
}
