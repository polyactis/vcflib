
include Makefile.common

CXX	= g++
CXXFLAGS	= -O3 -D_FILE_OFFSET_BITS=64 -fPIC -std=c++0x
LDFLAGS	=  -ltabix -lm -lz
SharedLibFlags	= -shared -fPIC

#CXXFLAGS = -O2
#CXXFLAGS = -pedantic -Wall -Wshadow -Wpointer-arith -Wcast-qual

#OBJ_DIR = ./
HEADERS = Variant.h \
		  split.h \
		  join.h
SOURCES = Variant.cpp \
		  split.cpp
OBJECTS= $(SOURCES:.cpp=.o)

# TODO
#vcfstats.cpp

BIN_SOURCES = vcfecho.cpp \
			  vcfaltcount.cpp \
			  vcfhetcount.cpp \
			  vcfhethomratio.cpp \
			  vcffilter.cpp \
			  vcf2tsv.cpp \
			  vcfgenotypes.cpp \
			  vcfannotategenotypes.cpp \
			  vcfcommonsamples.cpp \
			  vcfremovesamples.cpp \
			  vcfkeepsamples.cpp \
			  vcfsamplenames.cpp \
			  vcfgenotypecompare.cpp \
			  vcffixup.cpp \
			  vcfclassify.cpp \
			  vcfsamplediff.cpp \
			  vcfremoveaberrantgenotypes.cpp \
			  vcfrandom.cpp \
			  vcfparsealts.cpp \
			  vcfstats.cpp \
			  vcfflatten.cpp \
			  vcfprimers.cpp \
			  vcfnumalt.cpp \
			  vcfcleancomplex.cpp \
			  vcfintersect.cpp \
			  vcfannotate.cpp \
			  vcfallelicprimitives.cpp \
			  vcfoverlay.cpp \
			  vcfaddinfo.cpp \
			  vcfkeepinfo.cpp \
			  vcfkeepgeno.cpp \
			  vcfafpath.cpp \
			  vcfcountalleles.cpp \
			  vcflength.cpp \
			  vcfdistance.cpp \
			  vcfrandomsample.cpp \
			  vcfentropy.cpp \
			  vcfglxgt.cpp \
			  vcfroc.cpp \
			  vcfsom.cpp \
			  vcfcheck.cpp \
			  vcfstreamsort.cpp \
			  vcfuniq.cpp \
			  vcfuniqalleles.cpp \
			  vcfremap.cpp \
			  vcf2fasta.cpp \
			  vcfsitesummarize.cpp \
			  vcfbreakmulti.cpp \
			  vcfcreatemulti.cpp \
			  vcfevenregions.cpp \
			  vcfcat.cpp \
			  vcfgenosummarize.cpp \
			  vcfgenosamplenames.cpp \
			  vcfgeno2haplo.cpp

SRCS = $(BIN_SOURCES)
BINS = $(BIN_SOURCES:.cpp=)

TABIX = tabixpp/tabix.o

FASTAHACK = fastahack/Fasta.o

SMITHWATERMAN = smithwaterman/SmithWatermanGotoh.o 

REPEATS = smithwaterman/Repeats.o

INDELALLELE = smithwaterman/IndelAllele.o

DISORDER = smithwaterman/disorder.c

LEFTALIGN = smithwaterman/LeftAlign.o

FSOM = fsom/fsom.o

INCLUDES = -L. -Ltabixpp/

SharedLibTargets	= libvcf.so

all: $(SharedLibTargets) $(OBJECTS) $(BINS)


SSW = ssw.o ssw_cpp.o

ssw.o: ssw.h
ssw_cpp.o:ssw_cpp.h

openmp:
	$(MAKE) all CXXFLAGS="$(CXXFLAGS) -fopenmp -D HAS_OPENMP"

profiling:
	$(MAKE) all CXXFLAGS="$(CXXFLAGS) -g" all

gprof:
	$(MAKE) all CXXFLAGS="$(CXXFLAGS) -pg" all

$(OBJECTS): $(SOURCES) $(HEADERS) $(TABIX)
	$(CXX) $(*F).cpp -c -o $@ $(INCLUDES) $(IncludeDirs) $(CXXFLAGS) $(LDFLAGS)

$(TABIX):
	cd tabixpp && $(MAKE) all CFLAGS="$(CXXFLAGS)"

$(SMITHWATERMAN):
	cd smithwaterman && $(MAKE) all CFLAGS="$(CXXFLAGS)"

$(DISORDER): $(SMITHWATERMAN)

$(REPEATS): $(SMITHWATERMAN)

$(LEFTALIGN): $(SMITHWATERMAN)

$(INDELALLELE): $(SMITHWATERMAN)

$(FASTAHACK):
	cd fastahack && $(MAKE) all CXXFLAGS="$(CXXFLAGS)"

$(FSOM):
	cd fsom && $(CXX) $(CXXFLAGS) -c fsom.c $(IncludeDirs) -lm

$(BINS): $(BIN_SOURCES) $(OBJECTS) $(SMITHWATERMAN) $(FASTAHACK) $(DISORDER) $(LEFTALIGN) $(INDELALLELE) $(SSW) $(FSOM)
	$(CXX) $(OBJECTS) $(SMITHWATERMAN) $(REPEATS) $(DISORDER) $(LEFTALIGN) $(INDELALLELE) $(SSW) $(FASTAHACK) $(FSOM) tabixpp/tabix.o tabixpp/bgzf.o $@.cpp -o $@ $(INCLUDES) $(IncludeDirs) $(CXXFLAGS) $(LDFLAGS)

#$(SharedLibTargets): $(BIN_SOURCES) $(OBJECTS) $(SMITHWATERMAN) $(FASTAHACK) $(DISORDER) $(LEFTALIGN) $(INDELALLELE) $(SSW) $(FSOM)
$(SharedLibTargets): $(BIN_SOURCES) $(OBJECTS) $(TABIX)
	$(CXX) Variant.o split.o $(TABIX) tabixpp/bgzf.o -o $@ $(SharedLibFlags) $(INCLUDES) $(IncludeDirs) $(CXXFLAGS) $(LDFLAGS)

clean:
	rm -f $(BINS) $(OBJECTS) $(SharedLibTargets)
	rm -f ssw_cpp.o ssw.o
	cd tabixpp && make clean
	cd smithwaterman && make clean
	cd fastahack && make clean

.PHONY: clean all
