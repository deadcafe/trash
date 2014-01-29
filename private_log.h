#ifndef _PRIVATE_LOG_H_
#define	_PRIVATE_LOG_H_

#include <trema.h>

#if 0
# include <stdlib.h>
# include <syslog.h>
# define _log(pri_,fmt_,...)     fprintf(stdout,fmt_,##__VA_ARGS__)
# define LOG(pri_,fmt_,...)      _log((pri_),"%s:%d:%s() " fmt_ "\n", __FILE__,__LINE__,__func__, ##__VA_ARGS__)
# define CRITICAL(fmt_,...)	LOG(LOG_CRIT,fmt_,##__VA_ARGS__)
# define ERROR(fmt_,...)	LOG(LOG_ERROR,fmt_,##__VA_ARGS__)
# define WARN(fmt_,...)		LOG(LOG_WARNING,fmt_,##__VA_ARGS__)
# define NOTICE(fmt_,...)	LOG(LOG_NOTICE,fmt_,##__VA_ARGS__)
# define INFO(fmt_,...)		LOG(LOG_INFO,fmt_,##__VA_ARGS__)
# define DEBUG(fmt_,...)	LOG(LOG_DEBUG,fmt_,##__VA_ARGS__)
# ifdef ENABLE_TRACE
#  define TRACE(fmt_,...) 	LOG(LOG_DEBUG,fmt_,##__VA_ARGS__)
# else
#  define TRACE(fmt_,...)
# endif
#else
# define _CRITICAL(fmt_,...)	critical("%s:%d:%s() " fmt_,__FILE__,__LINE__,__func__, ##__VA_ARGS__)
# define _ERROR(fmt_,...)	error("%s:%d:%s() " fmt_,__FILE__,__LINE__,__func__, ##__VA_ARGS__)
# define _WARN(fmt_,...)	warn("%s:%d:%s() " fmt_,__FILE__,__LINE__,__func__, ##__VA_ARGS__)
# define _NOTICE(fmt_,...)	notice("%s:%d:%s() " fmt_,__FILE__,__LINE__,__func__, ##__VA_ARGS__)
# define _INFO(fmt_,...)	info("%s:%d:%s() " fmt_,__FILE__,__LINE__,__func__, ##__VA_ARGS__)
# define _DEBUG(fmt_,...)	debug("%s:%d:%s() " fmt_,__FILE__,__LINE__,__func__, ##__VA_ARGS__)
# define _TRACE(fmt_,...) 	debug("%s:%d:%s() " fmt_,__FILE__,__LINE__,__func__, ##__VA_ARGS__)

# define CRITICAL(fmt_,...)	_CRITICAL(fmt_,##__VA_ARGS__)
# define ERROR(fmt_,...)	_ERROR(fmt_,##__VA_ARGS__)
# define WARN(fmt_,...)		_WARN(fmt_,##__VA_ARGS__)
# define NOTICE(fmt_,...)	_NOTICE(fmt_,##__VA_ARGS__)
# define INFO(fmt_,...)		_INFO(fmt_,##__VA_ARGS__)
# define DEBUG(fmt_,...)	_DEBUG(fmt_,##__VA_ARGS__)
# ifdef ENABLE_TRACE
#  define TRACE(fmt_,...) 	_TRACE(fmt_,##__VA_ARGS__)
# else
#  define TRACE(fmt_,...)
# endif
#endif



#endif	/* !_PRIVATE_LOG_H_ */
