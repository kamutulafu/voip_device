/*
 * File      : rz.c
 * the implemention of receiving files from the remote computers  
 * through the zmodem protocol.
 * Change Logs:
 * Date           Author       Notes
 * 2011-03-29     itspy       
 * 2011-12-12     aozima       fixed syntax error.       
 */

#include "zdef.h"


void zr_start(char *path);
static rt_err_t zrec_init(rt_uint8_t *rxbuf, struct zfile *zf);
static rt_err_t zrec_files(struct zfile *zf);
static rt_err_t zwrite_file(rt_uint8_t *buf, rt_uint16_t size, struct zfile *zf);
static rt_err_t zrec_file_data(rt_uint8_t *buf, struct zfile *zf);;
static rt_err_t zrec_file(rt_uint8_t *rxbuf, struct zfile *zf);
static rt_err_t zget_file_info(char *name, struct zfile *zf);
static rt_err_t zwrite_file(rt_uint8_t *buf, rt_uint16_t size, struct zfile *zf);
static void zrec_ack_bibi(void);


/* start zmodem receive proccess */
void zr_start(char *path)
{
    struct zfile *zf;
    char *p,*q;
	rt_err_t res = -RT_ERROR;

	zf = rt_malloc(sizeof(struct zfile));
	if (zf == RT_NULL)
	{
	    rt_kprintf("[Zmodem Recv] zf: out of memory\r\n");
		return;
	}
	rt_kprintf("[Zmodem Recv] Starting receive session to path: %s\r\n", path);
	memset(zf, 0, sizeof(struct zfile));
    zf->fname = path;
	zf->fd = -1;
	uart_flush_input(CONFIG_ESP_CONSOLE_UART_NUM);
	res = zrec_files(zf);   
	p = zf->fname;
	for (;;)
	{
		q = strstr(p,"/");
		if (q == RT_NULL)  break;
		p = q+1;
	}	   
    if (res == RT_EOK)
    {		  
        rt_kprintf("[Zmodem Recv] Transfer completed: file=%s size=%ld bytes\r\n", p, zf->bytes_received);
		close(zf->fd);
		rt_free(zf->fname);
    }
    else
    {
        rt_kprintf("[Zmodem Recv] Transfer failed: file=%s\r\n", p);
		if (zf->fd >= 0)
		{
	        close(zf->fd);
	        unlink(zf->fname);    /* remove this file */ 
		}	
		rt_free(zf->fname);
    }
	rt_free(zf);
	/* waiting,clear console buffer */
	rt_thread_delay(pdMS_TO_TICKS(500));
	uint8_t dummy;
	while (uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &dummy, 1, 0) > 0);

	return ;
}

/* receiver init, wait for ack */
static rt_err_t zrec_init(rt_uint8_t *rxbuf, struct zfile *zf)
{
    rt_uint16_t err_cnt = 0;
	rt_err_t res = -RT_ERROR;

	rt_kprintf("[Zmodem Recv] Initializing receiver, waiting for file...\r\n");
	for (;;) 
	{
		zput_pos(0L);
		tx_header[ZF0] = ZF0_CMD;
		tx_header[ZF1] = ZF1_CMD;
		tx_header[ZF2] = ZF2_CMD;
		rt_kprintf("[Zmodem Recv] Sending ZRINIT...\r\n");
		zsend_hex_header(ZRINIT, tx_header);
again:
        res = zget_header(rx_header);
		rt_kprintf("[Zmodem Recv] zrec_init got header: %d\r\n", res);
		switch(res)
		{
		case ZFILE:						 
			 ZF0_CMD  = rx_header[ZF0];
			 ZF1_CMD  = rx_header[ZF1];
			 ZF2_CMD  = rx_header[ZF2];
			 ZF3_CMD  = rx_header[ZF3];
			 res = zget_data(rxbuf, RX_BUFFER_SIZE);
			 rt_kprintf("[Zmodem Recv] Got ZFILE data block, result: %d\r\n", res);
			 if (res == GOTCRCW)
			 {
	             if ((res =zget_file_info((char*)rxbuf,zf))!= RT_EOK) 
	             {
	                 zsend_hex_header(ZSKIP, tx_header);
		             return (res);
	             }
			     return RT_EOK;; 
			 }     
			 zsend_hex_header(ZNAK, tx_header);
			 goto again;
		case ZSINIT:
			 if (zget_data((rt_uint8_t*)Attn, ZATTNLEN) == GOTCRCW) 	  /* send zack */
			 {
				zsend_hex_header(ZACK, tx_header);
				goto again;
			 }
			 zsend_hex_header(ZNAK, tx_header);		     /* send znak */
			 goto again;
		case ZRQINIT:
			 continue;
		case ZEOF:
			 continue;
		case ZCOMPL:
			 goto again;
		case ZFIN:			     /* end file session */
			 zrec_ack_bibi(); 
			 return res;
		 default:
		      if (++err_cnt >1000) return -RT_ERROR;
		      continue;
		}
	}
}

/* receive files */
static rt_err_t zrec_files(struct zfile *zf)
{
	rt_uint8_t *rxbuf;
	rt_err_t res = -RT_ERROR;

	zinit_parameter();
	rxbuf = rt_malloc(RX_BUFFER_SIZE*sizeof(rt_uint8_t));
	if (rxbuf == RT_NULL)
	{
		 rt_kprintf("[Zmodem Recv] rxbuf: out of memory\r\n");
		 return -RT_ERROR;
	}
	rt_kprintf("[Zmodem Recv] Ready to receive files.\r\n");
	if ((res = zrec_init(rxbuf,zf))!= RT_EOK)
	{
	     rt_kprintf("[Zmodem Recv] Receiver initialization failed.\r\n");
		 rt_free(rxbuf);
		 return -RT_ERROR;
	}
	res = zrec_file(rxbuf,zf);
	if (res == ZFIN)
	{	
	    rt_free(rxbuf); 
	    return RT_EOK;	     /* if finish session */
	}
	else if (res == ZCAN)
	{
        rt_free(rxbuf);
		return ZCAN;        /* cancel by sender */
	}
	else
	{
	   zsend_can();
	   rt_free(rxbuf);
	   return res;
	}
}
/* receive file */
static rt_err_t zrec_file(rt_uint8_t *rxbuf, struct zfile *zf)
{
	rt_err_t res = - RT_ERROR;
	rt_uint16_t err_cnt = 0;

	do 
	{
		rt_kprintf("[Zmodem Recv] Requesting position (ZRPOS): %ld\r\n", (long)zf->bytes_received);
		zput_pos(zf->bytes_received);
		zsend_hex_header(ZRPOS, tx_header);
again:
        res = zget_header(rx_header);
		rt_kprintf("[Zmodem Recv] zrec_file got header: %d\r\n", res);
		switch (res) 
		{
		case ZDATA:
			 zget_pos(Rxpos);
			 rt_kprintf("[Zmodem Recv] Got ZDATA header, Rxpos: %ld, Expected: %ld\r\n", (long)Rxpos, (long)zf->bytes_received);
			 if (Rxpos != zf->bytes_received)
			 {
                 zsend_break(Attn);      
				 continue;
			 }
			 err_cnt = 0;
			 res = zrec_file_data(rxbuf,zf);
			 rt_kprintf("[Zmodem Recv] zrec_file_data returned: %d\r\n", res);
			 if (res == -RT_ERROR)
			 {	  
			     zsend_break(Attn);
			     continue;
			 }
			 else if (res == GOTCAN) return res;	
			 else goto again;	 
		case ZRPOS:
		     zget_pos(Rxpos);
			 rt_kprintf("[Zmodem Recv] Got ZRPOS header, new Rxpos: %ld\r\n", (long)Rxpos);
			 continue;
		case ZEOF:
		     err_cnt = 0;
		     zget_pos(Rxpos);
			 rt_kprintf("[Zmodem Recv] Got ZEOF header, Rxpos: %ld, bytes_received: %ld, total: %ld\r\n", (long)Rxpos, (long)zf->bytes_received, (long)zf->bytes_total);
			 if (Rxpos != zf->bytes_received  || Rxpos != zf->bytes_total) 
			 {
			     continue;
			 }							 
		     return (zrec_init(rxbuf,zf));    /* resend ZRINIT packet,ready to receive next file */
        case ZFIN:
			 rt_kprintf("[Zmodem Recv] Got ZFIN header. Closing session.\r\n");
			 zrec_ack_bibi(); 
			 return ZCOMPL; 
		case ZCAN:
             rt_kprintf("[Zmodem Recv] Sender cancelled file transfer.\r\n");
			 zf->bytes_received = 0L;		 /* throw the received data */  
		     return res;
		case ZSKIP:
			 rt_kprintf("[Zmodem Recv] Skip request received.\r\n");
			 return res;
		case -RT_ERROR:             
			 zsend_break(Attn);
			 continue;
		case ZNAK:
		case TIMEOUT:
		default: 
			continue;
		}
	} while(++err_cnt < 100);

	return res;
}

/* proccess file infomation */
static rt_err_t zget_file_info(char *name, struct zfile *zf)
{
	char *p;
	char *full_path;
	rt_uint16_t len;

	if (zf->fname == RT_NULL) 		       /* extract file path  */
	{
	    len = strlen(name)+2; 
	}
	else
	    len = strlen(zf->fname)+strlen(name)+2; 
	full_path = rt_malloc(len);
	if (full_path == RT_NULL)		 
	{
	    zsend_can();
		rt_kprintf("[Zmodem Recv] full_path: out of memory\r\n");
		return -RT_ERROR;
	}
	memset(full_path,0,len);

	if (zf->fname != RT_NULL) {
		strcpy(full_path, zf->fname);
		strcat(full_path, "/");
		rt_free(zf->fname);
	}
	strcat(full_path, name);
	zf->fname = full_path;

	p = name + strlen(name)+1;	   
	long int bytes_total = 0;
	long unsigned int ctime = 0;
	unsigned int mode = 0;
	sscanf((const char *)p, "%ld%lo%o", &bytes_total, &ctime, &mode);
	zf->bytes_total = bytes_total;
	zf->ctime = ctime;
	zf->mode = mode;

	zf->bytes_received   = 0L;
	rt_kprintf("[Zmodem Recv] Opening file for write: %s, total size: %ld\r\n", zf->fname, (long)zf->bytes_total);
	if ((zf->fd = open(zf->fname, O_CREAT|O_WRONLY|O_TRUNC, 0666)) < 0)	 /* create or replace exist file */
	{
	    zsend_can();
	    rt_kprintf("[Zmodem Recv] Can not create file: %s\r\n", zf->fname);	
		return -RT_ERROR;
	}

	return RT_EOK;
}

/* receive file data,continously, no ack */
static rt_err_t zrec_file_data(rt_uint8_t *buf, struct zfile *zf)
{
    rt_err_t res = -RT_ERROR;

more_data:
	res = zget_data(buf,RX_BUFFER_SIZE);
	rt_kprintf("[Zmodem Recv] Received data block, result: %d, count: %d, total: %ld/%ld\r\n", 
	           res, Rxcount, (long)(zf->bytes_received + Rxcount), (long)zf->bytes_total);
	switch(res)
	{
	case GOTCRCW:						   /* zack received */
		 zwrite_file(buf,Rxcount,zf);
		 zf->bytes_received += Rxcount;
		 zput_pos(zf->bytes_received);
		 zsend_line(XON);
		 zsend_hex_header(ZACK, tx_header);
		 return RT_EOK;
	case GOTCRCQ:
		 zwrite_file(buf,Rxcount,zf);
		 zf->bytes_received += Rxcount;
		 zput_pos(zf->bytes_received);
		 zsend_hex_header(ZACK, tx_header);
		 goto more_data;
	case GOTCRCG:
		 zwrite_file(buf,Rxcount,zf);
		 zf->bytes_received += Rxcount;
		 goto more_data;
	case GOTCRCE:
		 zwrite_file(buf,Rxcount,zf);
		 zf->bytes_received += Rxcount;
		 return RT_EOK;
	case GOTCAN:
	     rt_kprintf("[Zmodem Recv] Received GOTCAN error code.\r\n");
		 return res;
	case TIMEOUT:
	     return res;
    case -RT_ERROR:
	     zsend_break(Attn);
	     return res;
	default:
	     return res;
	}
}

/* write file */
static rt_err_t zwrite_file(rt_uint8_t *buf,rt_uint16_t size, struct zfile *zf)
{
	return (write(zf->fd,buf,size));
}

/* ack bibi */
static void zrec_ack_bibi(void)
{
	rt_uint8_t i;

	zput_pos(0L);
	rt_kprintf("[Zmodem Recv] Saying goodbye (ZFIN)...\r\n");
	for (i=0;i<3;i++) 
	{
		zsend_hex_header(ZFIN, tx_header);
		switch (zread_line(100)) 
		{
		case 'O':
			 zread_line(1);	
			 rt_kprintf("[Zmodem Recv] Got Over (O) confirmation.\r\n");
			 return;
		case RCDO:
			 return;
		case TIMEOUT:
		default:
			 break;
		}
	}
}

/* end of rz.c */
