/**********************************************
 * msgclient.h
 *
 *  Created on: 2011-07-28
 *    Author:   humingqing
 *    Comments: 实现与消息服务中心通信以及数据转换
 *********************************************/

#ifndef __MsgCLIENT_H__
#define __MsgCLIENT_H__

#include "interface.h"
#include <BaseClient.h>
#include <OnlineUser.h>
#include <time.h>
#include <Session.h>
#include <interpacker.h>
#include "statinfo.h"

#define MSG_USER_TAG  "MSGCLIENT"

class PConvert ;
class MsgClient : public BaseClient , public IMsgClient, public IUserNotify
{
public:
	MsgClient() ;
	virtual ~MsgClient() ;

	// 初始化
	virtual bool Init( ISystemEnv *pEnv );
	// 开始服务
	virtual bool Start( void );
	// 停止服务
	virtual void Stop();
	// 向MSG上传消息
	virtual void HandleUpMsgData( const char *code, const char *data, int len ) ;
	//更新车辆订阅，op为0是删除，为1是增加
	virtual void updateSub(const list<string> &macid, int op);
public:
	// 数据到来时处理
	virtual void on_data_arrived( socket_t *sock, const void* data, int len);
	// 断开连接处理
	virtual void on_dis_connection( socket_t *sock );
	// 为服务端使用
	virtual void on_new_connection( socket_t *sock, const char* ip, int port){};

	virtual void TimeWork();
	virtual void NoopWork();
	// 构建登陆处理
	virtual int build_login_msg( User &user, char *buf,int buf_len );
	// 通知用户状态变化
	virtual void NotifyUser( const _UserInfo &info , int op ) ;

protected:
	// 发送到指定地区码的用户
	bool SendDataToUser( const string &area_code, const char *data, int len ) ;
	// 纷发内部数据
	void HandleInnerData( socket_t *sock, const char *data, int len ) ;
	// 纷发登陆处理
	void HandleSession( socket_t *sock, const char *data, int len ) ;
	// 处理离线用户
	void HandleOfflineUsers( void );
	// 处理在线用户
	void HandleOnlineUsers(int timeval);
	// 从USERINFO转换为User处理
	void ConvertUser( const _UserInfo &info, User &user ) ;
	// 加载当前用户的订阅的数据
	void LoadSubscribe( User &user ) ;

private:
	// 环境指针
	ISystemEnv  	 *_pEnv ;
	// 最后一次访问时间
	time_t		 	  _last_handle_user_time ;
	// 在线用户处理
	OnlineUser   	  _online_user;
	// 协议转换部件
	PConvert	     *_convert ;
	// 会话管理，MAC与对应MSG的关系
	CSessionMgr  	  _session;
	// 拆包处理
	CInterSpliter     _packspliter ;
	// 处理MSG的统计处理
	CStatInfo		  _statinfo ;
};

#endif /* LISTCLIENT_H_ */
