// ***************************************************************************
// bamtools_fasta.cpp (c) 2010 Derek Barnett, Erik Garrison
// Marth Lab, Department of Biology, Boston College
// ---------------------------------------------------------------------------
// Last modified: 9 March 2012 (DB)
// ---------------------------------------------------------------------------
// Provides FASTA reading/indexing functionality.
// ***************************************************************************

#include "utils/bamtools_fasta.h"
using namespace BamTools;

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

struct Fasta::FastaPrivate
{

    struct FastaIndexData
    {
        std::string Name;
        int32_t Length;
        int64_t Offset;
        int32_t LineLength;
        int32_t
            ByteLength;  // LineLength + newline character(s) - varies on OS where file was generated
    };

    // data members
    FILE* Stream;
    bool IsOpen;

    FILE* IndexStream;
    bool HasIndex;
    bool IsIndexOpen;

    std::vector<FastaIndexData> Index;

    // ctor
    FastaPrivate();
    ~FastaPrivate();

    // 'public' API methods
    bool Close();
    bool CreateIndex(const std::string& indexFilename);
    bool GetBase(const int& refId, const int& position, char& base);
    bool GetSequence(const int& refId, const int& start, const int& stop, std::string& sequence);
    bool GetLength(const int& refId, int& length);
    bool Open(const std::string& filename, const std::string& indexFilename);

    // internal methods
private:
    void Chomp(char* sequence);
    bool GetNameFromHeader(const std::string& header, std::string& name);
    bool GetNextHeader(std::string& header);
    bool GetNextSequence(std::string& sequence, size_t count = -1);
    bool LoadIndexData();
    bool Rewind();
    bool WriteIndexData();
};

Fasta::FastaPrivate::FastaPrivate()
    : IsOpen(false)
    , HasIndex(false)
    , IsIndexOpen(false)
{}

Fasta::FastaPrivate::~FastaPrivate()
{
    Close();
}

// remove any trailing newlines
void Fasta::FastaPrivate::Chomp(char* sequence)
{

    static const int CHAR_LF = 10;
    static const int CHAR_CR = 13;

    int seqLength = std::strlen(sequence);
    if (seqLength == 0) {
        return;
    }
    --seqLength;  // ignore null terminator

    while (sequence[seqLength] == CHAR_LF || sequence[seqLength] == CHAR_CR) {
        sequence[seqLength] = 0;
        --seqLength;
        if (seqLength < 0) {
            break;
        }
    }
}

bool Fasta::FastaPrivate::Close()
{

    // close fasta file
    if (IsOpen) {
        fclose(Stream);
        IsOpen = false;
    }

    // close index file
    if (HasIndex && IsIndexOpen) {
        fclose(IndexStream);
        HasIndex = false;
        IsIndexOpen = false;
    }

    // return success
    return true;
}

bool Fasta::FastaPrivate::CreateIndex(const std::string& indexFilename)
{

    // check that file is open
    if (!IsOpen) {
        std::cerr << "FASTA error : cannot create index, FASTA file not open" << std::endl;
        return false;
    }

    // rewind FASTA file
    if (!Rewind()) {
        std::cerr << "FASTA error : could not rewind FASTA file" << std::endl;
        return false;
    }

    // clear out prior index data
    Index.clear();

    // -------------------------------------------
    // calculate lineLength & byteLength

    int lineLength = 0;
    int byteLength = 0;

    // skip over header
    char buffer[1024];
    if (fgets(buffer, 1024, Stream) == 0) {
        std::cerr << "FASTA error : could not read from file" << std::endl;
        return false;
    }
    if (feof(Stream)) {
        return false;
    }
    if (buffer[0] != '>') {
        std::cerr << "FASTA error : expected header ('>'), instead : " << buffer[0] << std::endl;
        return false;
    }

    // read in first line of sequence
    char c = fgetc(Stream);
    while ((c >= 0) && (c != '\n')) {
        ++byteLength;
        if (std::isgraph(c)) {
            ++lineLength;
        }
        c = fgetc(Stream);
    }
    ++byteLength;  // store newline

    // rewind FASTA file
    if (!Rewind()) {
        std::cerr << "FASTA error : could not rewind FASTA file" << std::endl;
        return false;
    }

    // iterate through fasta entries
    int currentId = 0;
    std::string header;
    std::string sequence;
    while (GetNextHeader(header)) {

        // ---------------------------
        // build index entry data
        FastaIndexData data;

        // store file offset of beginning of DNA sequence (after header)
        data.Offset = ftell64(Stream);

        // parse header, store sequence name in data.Name
        if (!GetNameFromHeader(header, data.Name)) {
            std::cerr << "FASTA error : could not parse read name from FASTA header" << std::endl;
            return false;
        }

        // retrieve FASTA sequence
        if (!GetNextSequence(sequence)) {
            std::cerr << "FASTA error : could not read in next sequence from FASTA file"
                      << std::endl;
            return false;
        }

        // store sequence length & line/byte lengths
        data.Length = sequence.length();
        data.LineLength = lineLength;
        data.ByteLength = byteLength;

        // store index entry
        Index.push_back(data);

        // update ref Id
        ++currentId;
    }

    // open index file
    if (!indexFilename.empty()) {
        IndexStream = fopen(indexFilename.c_str(), "wb");
        if (!IndexStream) {
            std::cerr << "FASTA error : Could not open " << indexFilename << " for writing."
                      << std::endl;
            return false;
        }
        IsIndexOpen = true;
    }

    // write index data
    if (!WriteIndexData()) {
        return false;
    }
    HasIndex = true;

    // close index file
    fclose(IndexStream);
    IsIndexOpen = false;

    // return succes status
    return true;
}

bool Fasta::FastaPrivate::GetBase(const int& refId, const int& position, char& base)
{

    // make sure FASTA file is open
    if (!IsOpen) {
        std::cerr << "FASTA error : file not open for reading" << std::endl;
        return false;
    }

    // use index if available
    if (HasIndex && !Index.empty()) {

        // validate reference id
        if ((refId < 0) || (refId >= (int)Index.size())) {
            std::cerr << "FASTA error: invalid refId specified: " << refId << std::endl;
            return false;
        }

        // retrieve reference index data
        const FastaIndexData& referenceData = Index.at(refId);

        // validate position
        if ((position < 0) || (position > referenceData.Length)) {
            std::cerr << "FASTA error: invalid position specified: " << position << std::endl;
            return false;
        }

        // calculate seek position & attempt jump
        const int64_t lines = position / referenceData.LineLength;
        const int64_t lineOffset = position % referenceData.LineLength;
        const int64_t seekTo =
            referenceData.Offset + (lines * referenceData.ByteLength) + lineOffset;
        if (fseek64(Stream, seekTo, SEEK_SET) != 0) {
            std::cerr << "FASTA error : could not seek in file" << std::endl;
            return false;
        }

        // set base & return success
        base = getc(Stream);
        return true;
    }

    // else plow through sequentially
    else {

        // rewind FASTA file
        if (!Rewind()) {
            std::cerr << "FASTA error : could not rewind FASTA file" << std::endl;
            return false;
        }

        // iterate through fasta entries
        int currentId = 0;
        std::string header;
        std::string sequence;

        // get first entry
        GetNextHeader(header);
        GetNextSequence(sequence);

        while (currentId != refId) {
            GetNextHeader(header);
            GetNextSequence(sequence);
            ++currentId;
        }

        // get desired base from sequence
        // TODO: error reporting on invalid position
        if (currentId == refId && (sequence.length() >= static_cast<std::size_t>(position))) {
            base = sequence.at(position);
            return true;
        }

        // could not get sequence
        return false;
    }

    // return success
    return true;
}

bool Fasta::FastaPrivate::GetNameFromHeader(const std::string& header, std::string& name)
{

    // get rid of the leading greater than sign
    std::string s = header.substr(1);

    // extract the first non-whitespace segment
    char* pName = (char*)s.data();
    unsigned int nameLen = (unsigned int)s.size();

    unsigned int start = 0;
    while ((pName[start] == 32) || (pName[start] == 9) || (pName[start] == 10) ||
           (pName[start] == 13)) {
        start++;
        if (start == nameLen) {
            break;
        }
    }

    unsigned int stop = start;
    if (stop < nameLen) {
        while ((pName[stop] != 32) && (pName[stop] != 9) && (pName[stop] != 10) &&
               (pName[stop] != 13)) {
            stop++;
            if (stop == nameLen) {
                break;
            }
        }
    }

    if (start == stop) {
        std::cerr << "FASTA error : could not parse read name from FASTA header" << std::endl;
        return false;
    }

    name = s.substr(start, stop - start).c_str();
    return true;
}

bool Fasta::FastaPrivate::GetNextHeader(std::string& header)
{

    // validate input stream
    if (!IsOpen || feof(Stream)) {
        return false;
    }

    // read in header line
    char buffer[1024];
    if (fgets(buffer, 1024, Stream) == 0) {
        std::cerr << "FASTA error : could not read from file" << std::endl;
        return false;
    }

    // make sure it's a FASTA header
    if (buffer[0] != '>') {
        std::cerr << "FASTA error : expected header ('>'), instead : " << buffer[0] << std::endl;
        return false;
    }

    // import buffer contents to header string
    std::stringstream headerBuffer;
    headerBuffer << buffer;
    header = headerBuffer.str();

    // return success
    return true;
}

bool Fasta::FastaPrivate::GetNextSequence(std::string& sequence, size_t count)
{

    sequence.clear();
    // validate input stream
    if (!IsOpen || feof(Stream)) {
        return false;
    }

    // read in sequence
    char buffer[1024];
    while (sequence.size() < count) {

        char ch = fgetc(Stream);
        ungetc(ch, Stream);
        if ((ch == '>') || feof(Stream)) {
            break;
        }

        if (fgets(buffer, 1024, Stream) == 0) {
            std::cerr << "FASTA error : could not read from file" << std::endl;
            return false;
        }

        Chomp(buffer);
        sequence.append(buffer);
    }

    // return success
    return true;
}

bool Fasta::FastaPrivate::GetSequence(const int& refId, const int& start, const int& stop,
                                      std::string& sequence)
{

    // make sure FASTA file is open
    if (!IsOpen) {
        std::cerr << "FASTA error : file not open for reading" << std::endl;
        return false;
    }

    // use index if available
    if (HasIndex && !Index.empty()) {

        // validate reference id
        if ((refId < 0) || (refId >= (int)Index.size())) {
            std::cerr << "FASTA error: invalid refId specified: " << refId << std::endl;
            return false;
        }

        // retrieve reference index data
        const FastaIndexData& referenceData = Index.at(refId);

        // validate stop position
        if ((start < 0) || (start > stop) || (stop > referenceData.Length)) {
            std::cerr << "FASTA error: invalid start/stop positions specified: " << start << ", "
                      << stop << std::endl;
            return false;
        }

        // seek to beginning of sequence data
        if (fseek64(Stream, referenceData.Offset, SEEK_SET) != 0) {
            std::cerr << "FASTA error : could not sek in file" << std::endl;
            return false;
        }

        // retrieve full sequence
        std::string fullSequence;
        const int seqLength = (stop - start) + 1;
        if (!GetNextSequence(fullSequence, stop + 1)) {
            std::cerr << "FASTA error : could not retrieve sequence from FASTA file" << std::endl;
            return false;
        }

        // set sub-sequence & return success
        sequence = fullSequence.substr(start, seqLength);
        return true;
    }

    // else plow through sequentially
    else {

        // rewind FASTA file
        if (!Rewind()) {
            std::cerr << "FASTA error : could not rewind FASTA file" << std::endl;
            return false;
        }

        // iterate through fasta entries
        int currentId = 0;
        std::string header;
        std::string fullSequence;

        // get first entry
        GetNextHeader(header);
        GetNextSequence(fullSequence);

        while (currentId != refId) {
            GetNextHeader(header);
            GetNextSequence(fullSequence);
            ++currentId;
        }

        // get desired substring from sequence
        // TODO: error reporting on invalid start/stop positions
        if (currentId == refId && (fullSequence.length() >= static_cast<std::size_t>(stop))) {
            const int seqLength = (stop - start) + 1;
            sequence = fullSequence.substr(start, seqLength);
            return true;
        }

        // could not get sequence
        return false;
    }

    // return success
    return true;
}

bool Fasta::FastaPrivate::GetLength(const int& refId, int& length)
{
    // make sure FASTA file is open
    if (!IsOpen) {
        std::cerr << "FASTA error : file not open for reading\n";
        return false;
    }

    // make sure index if available
    if (!HasIndex && Index.empty()) {
        std::cerr << "FASTA error : could not read from index file\n";
        return false;
    }

    length = Index.at(refId).Length;

    // return success
    return true;
}

bool Fasta::FastaPrivate::LoadIndexData()
{

    // skip if no index file available
    if (!IsIndexOpen) {
        return false;
    }

    // clear any prior index data
    Index.clear();

    char buffer[1024];
    std::stringstream indexBuffer;
    while (true) {

        char c = fgetc(IndexStream);
        if ((c == '\n') || feof(IndexStream)) {
            break;
        }
        ungetc(c, IndexStream);

        // clear index buffer
        indexBuffer.str(std::string());

        // read line from index file
        if (fgets(buffer, 1024, IndexStream) == 0) {
            std::cerr << "FASTA LoadIndexData() error : could not read from index file"
                      << std::endl;
            HasIndex = false;
            return false;
        }

        // store line in indexBuffer
        indexBuffer << buffer;

        // retrieve fasta index data from line
        FastaIndexData data;
        indexBuffer >> data.Name;
        indexBuffer >> data.Length;
        indexBuffer >> data.Offset;
        indexBuffer >> data.LineLength;
        indexBuffer >> data.ByteLength;

        // store index entry
        Index.push_back(data);
    }

    return true;
}

bool Fasta::FastaPrivate::Open(const std::string& filename, const std::string& indexFilename)
{

    bool success = true;

    // open FASTA filename
    Stream = fopen(filename.c_str(), "rb");
    if (!Stream) {
        std::cerr << "FASTA error: Could not open " << filename << " for reading" << std::endl;
        return false;
    }
    IsOpen = true;
    success &= IsOpen;

    // open index file if it exists
    if (!indexFilename.empty()) {
        IndexStream = fopen(indexFilename.c_str(), "rb");
        if (!IndexStream) {
            std::cerr << "FASTA error : Could not open " << indexFilename << " for reading."
                      << std::endl;
            return false;
        }
        IsIndexOpen = true;
        success &= IsIndexOpen;

        // attempt to load index data
        HasIndex = LoadIndexData();
        success &= HasIndex;
    }

    // return success status
    return success;
}

bool Fasta::FastaPrivate::Rewind()
{
    if (!IsOpen) {
        return false;
    }
    return (fseek64(Stream, 0, SEEK_SET) == 0);
}

bool Fasta::FastaPrivate::WriteIndexData()
{

    // skip if no index file available
    if (!IsIndexOpen) {
        return false;
    }

    // iterate over index entries
    bool success = true;
    std::stringstream indexBuffer;
    std::vector<FastaIndexData>::const_iterator indexIter = Index.begin();
    std::vector<FastaIndexData>::const_iterator indexEnd = Index.end();
    for (; indexIter != indexEnd; ++indexIter) {

        // clear stream
        indexBuffer.str(std::string());

        // write data to stream
        const FastaIndexData& data = (*indexIter);
        indexBuffer << data.Name << '\t' << data.Length << '\t' << data.Offset << '\t'
                    << data.LineLength << '\t' << data.ByteLength << std::endl;

        // write stream to file
        success &= (fputs(indexBuffer.str().c_str(), IndexStream) >= 0);
    }

    // return success status
    return success;
}

// --------------------------------
// Fasta implementation

Fasta::Fasta()
{
    d = new FastaPrivate;
}

Fasta::~Fasta()
{
    delete d;
    d = 0;
}

bool Fasta::Close()
{
    return d->Close();
}

bool Fasta::CreateIndex(const std::string& indexFilename)
{
    return d->CreateIndex(indexFilename);
}

bool Fasta::GetBase(const int& refId, const int& position, char& base)
{
    return d->GetBase(refId, position, base);
}

bool Fasta::GetSequence(const int& refId, const int& start, const int& stop, std::string& sequence)
{
    return d->GetSequence(refId, start, stop, sequence);
}

bool Fasta::Open(const std::string& filename, const std::string& indexFilename)
{
    return d->Open(filename, indexFilename);
}

bool Fasta::GetLength(const int& refId, int& length)
{
    return d->GetLength(refId, length);
}
