
.PATH:	${.CURDIR}/..

PROG=	example
MAN=	

CFLAGS+=-I/usr/local/include -I${.CURDIR}/..
LDFLAGS+=-L/usr/local/lib -lev

SRCS=	example.c
SRCS+=	umqtt.c
SRCS+=	buffer.c log.c ssl.c utils.c

.include <bsd.prog.mk>
