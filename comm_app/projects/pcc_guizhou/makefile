############################################################
## makefile of libunisql.a
############################################################
.SUFFIXES: .cpp

SOURCES  = ../share/TProto/ProtoParse.cpp ../share/cache/msgcache.cpp   
SOURCES += pccserver.cpp pasclient.cpp pconvert.cpp msgclient.cpp  systemenv.cpp pccmain.cpp
SOURCES += statinfo.cpp userloader.cpp
PROGRAM  = pcc_guizhou
CLIB     = 

##SPECIAL_MACRO_FLAGS = -D_PCC_DEBUG_VER
SPECIAL_CCFLAGS = 
SPECIAL_INCL_PATH = -I../include -I../rediscache -I../share/TProto -I../share/coder -I../share/cache
SPECIAL_LIB_PATH = -L../rediscache -L$(DBLIBS_HOME)/redis/lib

SPECIAL_LIBS = -lctfobase -lrediscache -lhiredis

include ${MTRANS_PRJ_HOME}/makefile.include
