#include "Variant.h"
#include "convert.h"
#include "join.h"
#include "split.h"
#include "fastahack/Fasta.h"
#include <set>
#include <vector>
#include <getopt.h>

using namespace std;
using namespace vcf;


// Attempts to left-realign all the indels represented by the alignment cigar.
//
// This is done by shifting all indels as far left as they can go without
// mismatch, then merging neighboring indels of the same class.  leftAlign
// updates the alignment cigar with changes, and returns true if realignment
// changed the alignment cigar.
//
// To left-align, we move multi-base indels left by their own length as long as
// the preceding bases match the inserted or deleted sequence.  After this
// step, we handle multi-base homopolymer indels by shifting them one base to
// the left until they mismatch the reference.
//
// To merge neighboring indels, we iterate through the set of left-stabilized
// indels.  For each indel we add a new cigar element to the new cigar.  If a
// deletion follows a deletion, or an insertion occurs at the same place as
// another insertion, we merge the events by extending the previous cigar
// element.
//
// In practice, we must call this function until the alignment is stabilized.

using namespace std;

class VCFIndelAllele {
    friend ostream& operator<<(ostream&, const VCFIndelAllele&);
    friend bool operator==(const VCFIndelAllele&, const VCFIndelAllele&);
    friend bool operator!=(const VCFIndelAllele&, const VCFIndelAllele&);
    friend bool operator<(const VCFIndelAllele&, const VCFIndelAllele&);
public:
    bool insertion;
    int length;
    int position;
    int readPosition;
    string sequence;

    bool homopolymer(void);

    VCFIndelAllele(bool i, int l, int p, int rp, string s)
        : insertion(i), length(l), position(p), readPosition(rp), sequence(s)
    { }
};

bool FBhomopolymer(string sequence);
ostream& operator<<(ostream& out, const VCFIndelAllele& indel);
bool operator==(const VCFIndelAllele& a, const VCFIndelAllele& b);
bool operator!=(const VCFIndelAllele& a, const VCFIndelAllele& b);
bool operator<(const VCFIndelAllele& a, const VCFIndelAllele& b);

bool VCFIndelAllele::homopolymer(void) {
    string::iterator s = sequence.begin();
    char c = *s++;
    while (s != sequence.end()) {
        if (c != *s++) return false;
    }
    return true;
}

bool FBhomopolymer(string sequence) {
    string::iterator s = sequence.begin();
    char c = *s++;
    while (s != sequence.end()) {
        if (c != *s++) return false;
    }
    return true;
}

ostream& operator<<(ostream& out, const VCFIndelAllele& indel) {
    string t = indel.insertion ? "i" : "d";
    out << t <<  ":" << indel.position << ":" << indel.readPosition << ":" << indel.sequence;
    return out;
}

bool operator==(const VCFIndelAllele& a, const VCFIndelAllele& b) {
    return (a.insertion == b.insertion
            && a.length == b.length
            && a.position == b.position
            && a.sequence == b.sequence);
}

bool operator!=(const VCFIndelAllele& a, const VCFIndelAllele& b) {
    return !(a==b);
}

bool operator<(const VCFIndelAllele& a, const VCFIndelAllele& b) {
    ostringstream as, bs;
    as << a;
    bs << b;
    return as.str() < bs.str();
}


class AltAlignment {
public:
    unsigned int pos;
    string seq;
    vector<pair<int, string> > cigar;
    AltAlignment(unsigned int& p,
                 string& s,
                 string& c) {
        pos = p;
        seq = s;
        cigar = splitCigar(c);
    }
};

void getAlignment(Variant& var, FastaReference& reference, string& ref, vector<AltAlignment>& alignments, int window) {
    
    // default alignment params
    float matchScore = 10.0f;
    float mismatchScore = -9.0f;
    float gapOpenPenalty = 15.0f;
    float gapExtendPenalty = 6.66f;

    // establish reference sequence
    string leftFlank = reference.getSubSequence(var.sequenceName, var.zeroBasedPosition() - window/2, window/2);
    string rightFlank = reference.getSubSequence(var.sequenceName, var.zeroBasedPosition() + var.ref.size(), window/2);
    ref = leftFlank + var.ref + rightFlank;

    // and iterate through the alternates, generating alignments
    for (vector<string>::iterator a = var.alt.begin(); a != var.alt.end(); ++a) {
        string alt = leftFlank + *a + rightFlank;
        CSmithWatermanGotoh sw(matchScore, mismatchScore, gapOpenPenalty, gapExtendPenalty);
        unsigned int referencePos;
        string cigar;
        sw.Align(referencePos, cigar, ref, alt);
        alignments.push_back(AltAlignment(referencePos, alt, cigar));
    }
}


bool stablyLeftAlign(string& alternateSequence, string referenceSequence, int maxiterations = 20, bool debug = false);
int countMismatches(string& alternateSequence, string referenceSequence);

bool leftAlign(string& alternateSequence, vector<pair<int, string> >& cigar, string& referenceSequence, bool debug = false) {

    int arsOffset = 0; // pointer to insertion point in aligned reference sequence
    string alignedReferenceSequence = referenceSequence;
    int aabOffset = 0;
    string alignmentAlignedBases = alternateSequence;

    // store information about the indels
    vector<VCFIndelAllele> indels;

    int rp = 0;  // read position, 0-based relative to read
    int sp = 0;  // sequence position

    string softBegin;
    string softEnd;

    stringstream cigar_before, cigar_after;
    for (vector<pair<int, string> >::const_iterator c = cigar.begin();
        c != cigar.end(); ++c) {
        unsigned int l = c->first;
        char t = c->second.at(0);
        cigar_before << l << t;
        if (t == 'M') { // match or mismatch
            sp += l;
            rp += l;
        } else if (t == 'D') { // deletion
            indels.push_back(VCFIndelAllele(false, l, sp, rp, referenceSequence.substr(sp, l)));
            alignmentAlignedBases.insert(rp + aabOffset, string(l, '-'));
            aabOffset += l;
            sp += l;  // update reference sequence position
        } else if (t == 'I') { // insertion
            indels.push_back(VCFIndelAllele(true, l, sp, rp, alternateSequence.substr(rp, l)));
            alignedReferenceSequence.insert(sp + softBegin.size() + arsOffset, string(l, '-'));
            arsOffset += l;
            rp += l;
        } else if (t == 'S') { // soft clip, clipped sequence present in the read not matching the reference
            // remove these bases from the refseq and read seq, but don't modify the alignment sequence
            if (rp == 0) {
                alignedReferenceSequence = string(l, '*') + alignedReferenceSequence;
                softBegin = alignmentAlignedBases.substr(0, l);
            } else {
                alignedReferenceSequence = alignedReferenceSequence + string(l, '*');
                softEnd = alignmentAlignedBases.substr(alignmentAlignedBases.size() - l, l);
            }
            rp += l;
        } else if (t == 'H') { // hard clip on the read, clipped sequence is not present in the read
        } else if (t == 'N') { // skipped region in the reference not present in read, aka splice
            sp += l;
        }
    }


    int alignedLength = sp;

    LEFTALIGN_DEBUG("| " << cigar_before.str() << endl
       << "| " << alignedReferenceSequence << endl
       << "| " << alignmentAlignedBases << endl);

    // if no indels, return the alignment
    if (indels.empty()) { return false; }

    // for each indel, from left to right
    //     while the indel sequence repeated to the left and we're not matched up with the left-previous indel
    //         move the indel left

    vector<VCFIndelAllele>::iterator previous = indels.begin();
    for (vector<VCFIndelAllele>::iterator id = indels.begin(); id != indels.end(); ++id) {

        // left shift by repeats
        //
        // from 1 base to the length of the indel, attempt to shift left
        // if the move would cause no change in alignment optimality (no
        // introduction of mismatches, and by definition no change in gap
        // length), move to the new position.
        // in practice this moves the indel left when we reach the size of
        // the repeat unit.
        //
        int steppos, readsteppos;
        VCFIndelAllele& indel = *id;
        int i = 1;
        while (i <= indel.length) {

            int steppos = indel.position - i;
            int readsteppos = indel.readPosition - i;

#ifdef VERBOSE_DEBUG
            if (debug) {
                if (steppos >= 0 && readsteppos >= 0) {
                    cerr << referenceSequence.substr(steppos, indel.length) << endl;
                    cerr << alternateSequence.substr(readsteppos, indel.length) << endl;
                    cerr << indel.sequence << endl;
                }
            }
#endif
            while (steppos >= 0 && readsteppos >= 0
                   && indel.sequence == referenceSequence.substr(steppos, indel.length)
                   && indel.sequence == alternateSequence.substr(readsteppos, indel.length)
                   && (id == indels.begin()
                       || (previous->insertion && steppos >= previous->position)
                       || (!previous->insertion && steppos >= previous->position + previous->length))) {
                LEFTALIGN_DEBUG((indel.insertion ? "insertion " : "deletion ") << indel << " shifting " << i << "bp left" << endl);
                indel.position -= i;
                indel.readPosition -= i;
                steppos = indel.position - i;
                readsteppos = indel.readPosition - i;
            }
            do {
                ++i;
            } while (i <= indel.length && indel.length % i != 0);
        }

        // left shift indels with exchangeable flanking sequence
        //
        // for example:
        //
        //    GTTACGTT           GTTACGTT
        //    GT-----T   ---->   G-----TT
        //
        // GTGTGACGTGT           GTGTGACGTGT
        // GTGTG-----T   ---->   GTG-----TGT
        //
        // GTGTG-----T           GTG-----TGT
        // GTGTGACGTGT   ---->   GTGTGACGTGT
        //
        //
        steppos = indel.position - 1;
        readsteppos = indel.readPosition - 1;
        while (steppos >= 0 && readsteppos >= 0
               && alternateSequence.at(readsteppos) == referenceSequence.at(steppos)
               && alternateSequence.at(readsteppos) == indel.sequence.at(indel.sequence.size() - 1)
               && (id == indels.begin()
                   || (previous->insertion && indel.position - 1 >= previous->position)
                   || (!previous->insertion && indel.position - 1 >= previous->position + previous->length))) {
            LEFTALIGN_DEBUG((indel.insertion ? "insertion " : "deletion ") << indel << " exchanging bases " << 1 << "bp left" << endl);
            indel.sequence = indel.sequence.at(indel.sequence.size() - 1) + indel.sequence.substr(0, indel.sequence.size() - 1);
            indel.position -= 1;
            indel.readPosition -= 1;
            steppos = indel.position - 1;
            readsteppos = indel.readPosition - 1;
        }
        // tracks previous indel, so we don't run into it with the next shift
        previous = id;
    }

    // bring together floating indels
    // from left to right
    // check if we could merge with the next indel
    // if so, adjust so that we will merge in the next step
    if (indels.size() > 1) {
        previous = indels.begin();
        for (vector<VCFIndelAllele>::iterator id = (indels.begin() + 1); id != indels.end(); ++id) {
            VCFIndelAllele& indel = *id;
            // parsimony: could we shift right and merge with the previous indel?
            // if so, do it
            int prev_end_ref = previous->insertion ? previous->position : previous->position + previous->length;
            int prev_end_read = !previous->insertion ? previous->readPosition : previous->readPosition + previous->length;
            if (previous->insertion == indel.insertion
                    && ((previous->insertion
                        && (previous->position < indel.position
                        && previous->readPosition + previous->readPosition < indel.readPosition))
                        ||
                        (!previous->insertion
                        && (previous->position + previous->length < indel.position)
                        && (previous->readPosition < indel.readPosition)
                        ))) {
                if (previous->homopolymer()) {
                    string seq = referenceSequence.substr(prev_end_ref, indel.position - prev_end_ref);
                    string readseq = alternateSequence.substr(prev_end_read, indel.position - prev_end_ref);
                    LEFTALIGN_DEBUG("seq: " << seq << endl << "readseq: " << readseq << endl);
                    if (previous->sequence.at(0) == seq.at(0)
                            && FBhomopolymer(seq)
                            && FBhomopolymer(readseq)) {
                        LEFTALIGN_DEBUG("moving " << *previous << " right to " 
                                << (indel.insertion ? indel.position : indel.position - previous->length) << endl);
                        previous->position = indel.insertion ? indel.position : indel.position - previous->length;
                    }
                } 
                else {
                    int pos = previous->position;
                    while (pos < (int) referenceSequence.length() &&
                            ((previous->insertion && pos + previous->length <= indel.position)
                            ||
                            (!previous->insertion && pos + previous->length < indel.position))
                            && previous->sequence 
                                == referenceSequence.substr(pos + previous->length, previous->length)) {
                        pos += previous->length;
                    }
                    if (pos < previous->position &&
                        ((previous->insertion && pos + previous->length == indel.position)
                        ||
                        (!previous->insertion && pos == indel.position - previous->length))
                       ) {
                        LEFTALIGN_DEBUG("right-merging tandem repeat: moving " << *previous << " right to " << pos << endl);
                        previous->position = pos;
                    }
                }
            }
            previous = id;
        }
    }

    // for each indel
    //     if ( we're matched up to the previous insertion (or deletion) 
    //          and it's also an insertion or deletion )
    //         merge the indels
    //
    // and simultaneously reconstruct the cigar

    vector<pair<int, string> > newCigar;

    if (!softBegin.empty()) {
        newCigar.push_back(make_pair(softBegin.size(), "S"));
    }

    vector<VCFIndelAllele>::iterator id = indels.begin();
    VCFIndelAllele last = *id++;
    if (last.position > 0) {
        newCigar.push_back(make_pair(last.position, "M"));
        newCigar.push_back(make_pair(last.length, (last.insertion ? "I" : "D")));
    } else {
        newCigar.push_back(make_pair(last.length, (last.insertion ? "I" : "D")));
    }
    int lastend = last.insertion ? last.position : (last.position + last.length);
    LEFTALIGN_DEBUG(last << ",");

    for (; id != indels.end(); ++id) {
        VCFIndelAllele& indel = *id;
        LEFTALIGN_DEBUG(indel << ",");
        if (indel.position < lastend) {
            cerr << "impossibility?: indel realigned left of another indel" << endl
                 << referenceSequence << endl << alternateSequence << endl;
            exit(1);
        } else if (indel.position == lastend && indel.insertion == last.insertion) {
            pair<int, string>& op = newCigar.back();
            op.first += indel.length;
        } else if (indel.position >= lastend) {  // also catches differential indels, but with the same position
            newCigar.push_back(make_pair(indel.position - lastend, "M"));
            newCigar.push_back(make_pair(indel.length, (indel.insertion ? "I" : "D")));
        }
        last = *id;
        lastend = last.insertion ? last.position : (last.position + last.length);
    }
    
    if (lastend < alignedLength) {
        newCigar.push_back(make_pair(alignedLength - lastend, "M"));
    }

    if (!softEnd.empty()) {
        newCigar.push_back(make_pair(softEnd.size(), "S"));
    }

    LEFTALIGN_DEBUG(endl);

    cigar = newCigar;

    for (vector<pair<int, string> >::const_iterator c = cigar.begin();
        c != cigar.end(); ++c) {
        unsigned int l = c->first;
        char t = c->second.at(0);
        cigar_after << l << t;
    }
    LEFTALIGN_DEBUG(cigar_after.str() << endl);

    // check if we're realigned
    if (cigar_after.str() == cigar_before.str()) {
        return false;
    } else {
        return true;
    }

}

// Iteratively left-aligns the indels in the alignment until we have a stable
// realignment.  Returns true on realignment success or non-realignment.
// Returns false if we exceed the maximum number of realignment iterations.
//
bool stablyLeftAlign(string& alternateSequence, string referenceSequence, int maxiterations, bool debug) {

    if (!leftAlign(alignment, referenceSequence, debug)) {

        return true;

    } else {

        while (leftAlign(alignment, referenceSequence, debug) && --maxiterations > 0) {
        }

        if (maxiterations <= 0) {
            return false;
        } else {
            return true;
        }

    }

}


void printSummary(char** argv) {
    cerr << "usage: " << argv[0] << " [options] [file]" << endl
         << endl
         << "options:" << endl
         << "    -r, --reference FILE  Use this reference as a basis for realignment." << endl
         << "    -w, --window N        Use a window of this many bp when left aligning (50)." << endl
         << endl
         << "Left-aligns variants in the specified input file or stdin." << endl;
    exit(0);
}

int main(int argc, char** argv) {

    int window = 50;
    VariantCallFile variantFile;
    string fastaFileName;

    int c;
    while (true) {
        static struct option long_options[] =
            {
                /* These options set a flag. */
                //{"verbose", no_argument,       &verbose_flag, 1},
                {"help", no_argument, 0, 'h'},
                {"reference", required_argument, 0, 'r'},
                {"window", required_argument, 0, 'w'},
                {0, 0, 0, 0}
            };
        /* getopt_long stores the option index here. */
        int option_index = 0;

        c = getopt_long (argc, argv, "hmt:",
                         long_options, &option_index);

        if (c == -1)
            break;

        switch (c) {

	    case 'r':
            fastaFileName = optarg;
            break;

	    case 'w':
            window = atoi(optarg);
            break;

        case '?':
            printSummary(argv);
            exit(1);
            break;

        case 'h':
            printSummary(argv);
            break;

        default:
            abort ();
        }
    }

    if (optind < argc) {
        string filename = argv[optind];
        variantFile.open(filename);
    } else {
        variantFile.open(std::cin);
    }

    if (!variantFile.is_open()) {
        cerr << "could not open VCF file" << endl;
        exit(1);
    }

    FastaReference freference;
    if (fastaFileName.empty()) {
        cerr << "a reference is required" << endl;
        exit(1);
    } else {
        freference.open(fastaFileName);
    }

    /*
    variantFile.addHeaderLine("##INFO=<ID=TYPE,Number=A,Type=String,Description=\"The type of allele, either snp, mnp, ins, del, or complex.\">");
    variantFile.addHeaderLine("##INFO=<ID=LEN,Number=A,Type=Integer,Description=\"allele length\">");
    if (!parseFlag.empty()) {
        variantFile.addHeaderLine("##INFO=<ID="+parseFlag+",Number=0,Type=Flag,Description=\"The allele was parsed using vcfallelicprimitives.\">");
    }
    */
    cout << variantFile.header << endl;

    Variant var(variantFile);
    while (variantFile.getNextVariant(var)) {

        vector<AltAlignments> alignments;
        getAlignment(var, reference, alignments);

        //cout << var << endl;

        // for each parsedalternate, get the position
        // build a new vcf record for that position
        // unless we are already at the position !
        // take everything which is unique to that allele (records) and append it to the new record
        // then handle genotypes; determine the mapping between alleleic primitives and convert to phased haplotypes
        // this means taking all the parsedAlternates and, for each one, generating a pattern of allele indecies corresponding to it

        

        //for (vector<Variant>::iterator v = variants.begin(); v != variants.end(); ++v) {
        for (map<long unsigned int, Variant>::iterator v = variants.begin(); v != variants.end(); ++v) {
            cout << v->second << endl;
        }
    }

    return 0;

}

