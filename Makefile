LIB=	umqtt
MAN=	

CFLAGS+=-I/usr/local/include

HDRS=	umqtt.h
SRCS=	umqtt.c
SRCS+=	buffer.c log.c utils.c

includes:
	@cd ${.CURDIR}; for i in $(HDRS); do \
	    j="cmp -s $$i ${DESTDIR}/usr/include/$$i || \
		${INSTALL} ${INSTALL_COPY} -o ${BINOWN} -g ${BINGRP} -m 444 $$i \
		${DESTDIR}/usr/include"; \
	    echo $$j; \
	    eval "$$j"; \
	done

.include <bsd.lib.mk>
