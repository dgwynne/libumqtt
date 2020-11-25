
.PATH:	${.CURDIR}/..

PROG=	example
MAN=	

SRCS=	example.c
SRCS+=	umqtt.c
SRCS+=	buffer.c log.c ssl.c utils.c

CFLAGS+=-I/usr/local/include -I${.CURDIR}/..

LDADD=  -levent
DPADD=  ${LIBEVENT}

DEBUG=-g

.include <bsd.prog.mk>
