/*
 * pccserver.cpp
 *
 *  Created on: 2011-11-28
 *      Author: humingqing
 *      这里数据来源两部分，一部分来自于各省平台的下行数据，一部分来自内部的下行数据
 *      省平台下行数据需要交由企来平台处理，而企业平台内部下行数据则需要交由省平台处理。
 */

#include "pccserver.h"
#include <comlog.h>
#include <crc16.h>
#include <netutil.h>

CPccServer::CPccServer()
{
	_thread_num = 15 ;
	_areaids    = "";
}

CPccServer::~CPccServer()
{
	Stop() ;
}

// 初始化
bool CPccServer::Init( ISystemEnv *pEnv )
{
	_pEnv = pEnv ;

	char value[1024] = {0} ;
	if ( ! _pEnv->GetString("pcc_listen_ip", value) )
	{
		printf("get pcc_listen_ip failed!\n");
		return false ;
	}

	_listen_ip = value ;

	int port = 0 ;
	if ( !_pEnv->GetInteger("pcc_listen_port", port ) )
	{
		printf("get pcc_listen_port failed!\n");
		return false ;
	}
	_listen_port = port ;


	int thread = 0 ;
	if ( ! _pEnv->GetInteger("pcc_recv_thread" , thread ) ){
		thread = 10 ;
	}
	_thread_num = thread ;

	return true ;
}

// 开始线程
bool CPccServer::Start( void )
{
	return StartServer( _listen_port, _listen_ip.c_str() , _thread_num ) ;
}

// 停止处理
void CPccServer::Stop( void )
{
	StopServer() ;
}

// 实现服务必需的接口
void CPccServer::on_data_arrived( socket_t *sock, const void* data, int len )
{
	if ( len <= 0 || data == NULL ){
		OUT_ERROR( sock->_szIp, sock->_port, NULL, "fd %d , data error length %d" , sock->_fd ,len ) ;
		OUT_HEX( sock->_szIp, sock->_port, "PccServer" , (const char *)data, len ) ;
		return ;
	}

	const char *ptr = (const char *)data ;
	if ( *ptr == '[' ) { //0x5B  包头
		// 如果809需要解码处理
		C5BCoder coder ;
		if ( ! coder.Decode( (const char *)data, len ) ) {
			OUT_WARNING( sock->_szIp, sock->_port, NULL, "Except packet header or tail" ) ;
			OUT_HEX( sock->_szIp, sock->_port, NULL, (const char *) data, len ) ;
			return;
		}
		// 纷发下行数据
		HandleOutData( sock, coder.GetData(), coder.GetSize() );
	}
}

// 处理809协议的数据
void CPccServer::HandleOutData( socket_t *sock, const char *data, int len )
{
	// 处理809
	if (data == NULL || len < (int)sizeof(Header)) {
		OUT_HEX( sock->_szIp, sock->_port, "PccServer" , (const char *)data, len ) ;
		return;
	}

	Header *header = (Header *) data;
	unsigned int access_code = ntouv32(header->access_code);
	string str_access_code = uitodecstr(access_code);

	unsigned short msg_type = ntouv16(header->msg_type);

	const char *ip    = sock->_szIp ;
	unsigned int port = sock->_port ;

	// 处理加解密数据
	EncryptData( (unsigned char*) data , len , false ) ;

	OUT_RECV( ip, port, str_access_code.c_str(), "%s,from pccserver", _proto_parse.Decoder(data,len).c_str() ) ;
	OUT_HEX( ip, port, str_access_code.c_str(), data, len ) ;

	if (msg_type == DOWN_CONNECT_REQ){
		// 连接请求
		User user = _online_user.GetUserByUserId(str_access_code);
		if ( user._fd != NULL && user._fd != sock ) {
			CloseSocket(user._fd) ;
		}

		DownConnectReq* req = (DownConnectReq*) data;

		user._fd          = sock;
		user._ip          = ip ;
		user._port        = port ;
		user._login_time  = time(0);
		user._msg_seq     = 0;
		user._user_id     = str_access_code ;
		user._access_code = access_code ;
		user._user_state  = User::ON_LINE;

		//一定要设置这个，因为在下行链路还没有建立起来的时候，第一次登录的时候上的那个是不能执行的。
		user._last_active_time = time(0);

		// 添加用户
		if ( ! _online_user.AddUser( str_access_code, user ) ){
			_online_user.SetUser( str_access_code, user  ) ;
		}

		DownConnectRsp resp;
		resp.header.msg_seq 	= ntouv16(_proto_parse.get_next_seq());
		resp.header.access_code = req->header.access_code;
		resp.result = 0;

		if ( SendCrcData( sock,(const char *)&resp,sizeof(resp) ) ) {
			OUT_INFO(ip,port,str_access_code.c_str(),"DOWN_CONNECT_REQ: send DOWN_CONNECT_RSP downlink is online");
		} else {
			OUT_ERROR(ip,port,str_access_code.c_str(),"DOWN_CONNECT_REQ: send DOWN_CONNECT_RSP downlink is online failed");
		}
		// 更新连接状态
		_pEnv->GetPasClient()->UpdateSlaveConn( access_code , CONN_CONNECT ) ;

		return;
	}

	User user = _online_user.GetUserBySocket( sock );

	if (user._user_id.empty()){
		OUT_ERROR(ip,port,str_access_code.c_str(),"msg type %04x, user havn't login,close it %d", msg_type, sock->_fd );
		CloseSocket( sock );
		return;
	}

	// 如果为心跳
	if (msg_type == DOWN_LINKTEST_REQ){
		OUT_RECV(ip,port,user._user_id.c_str(),"DOWN_LINKTEST_REQ");
		DownLinkTestRsp resp;
		resp.header.access_code = header->access_code;
		resp.header.msg_seq 	= ntouv32(_proto_parse.get_next_seq());

		if ( SendCrcData( sock, (const char*) &resp, sizeof(resp) ) )
			OUT_SEND(ip,port,user._user_id.c_str(),"DOWN_LINKTEST_RSP");
		else
			OUT_ERROR(ip,port,user._user_id.c_str(),"DOWN_LINKTEST_RSP") ;
	}
	else if (msg_type == DOWN_DISCONNECT_REQ ) {  // 从链路注销请求
		OUT_RECV(ip,port,user._user_id.c_str(),get_type(msg_type));

		DownDisconnectRsp resp ;
		resp.header.msg_len     = ntouv32( sizeof(resp) ) ;
		resp.header.msg_type    = ntouv16( DOWN_DISCONNECT_RSP ) ;
		resp.header.access_code = header->access_code ;
		resp.header.msg_seq     = ntouv32( _proto_parse.get_next_seq() ) ;

		if ( SendCrcData( sock, (const char*) &resp, sizeof(resp) ) )
			OUT_SEND(ip,port,user._user_id.c_str(),"DOWN_DISCONNECT_RSP");
		else
			OUT_ERROR(ip,port,user._user_id.c_str(),"DOWN_DISCONNECT_RSP") ;

		// 这里由主链路来主动关闭连接，置离线状态由线程自动回收链路
		// user._user_state = User::OFF_LINE ;
	}
	else if (msg_type == DOWN_DISCONNECT_INFORM){ // 主链路消息
		//不管它，由定时线程来完成。
		OUT_RECV(ip,port,user._user_id.c_str(),get_type(msg_type));
	}
	else if (msg_type == DOWN_CLOSELINK_INFORM ){ // 来自主链路

	}
	else{
		// 直接到CLIENT处理
		_pEnv->GetPasClient()->HandlePasDownData( access_code, data, len ) ;
	}

	user._last_active_time = time(0);
	_online_user.SetUser(user._user_id,user);
}

void CPccServer::on_dis_connection( socket_t *sock )
{
	//专门处理底层的链路突然断开的情况，不处理超时和正常流程下的断开情况。
	User user = _online_user.GetUserBySocket( sock );
	if ( user._user_id.empty() ) {
		return ;
	}
	// 如果从链路的连接
	if ( user._access_code > 0 ) {
		// 更新连接状态
		_pEnv->GetPasClient()->UpdateSlaveConn( user._access_code , CONN_DISCONN ) ;
	}
	OUT_WARNING( sock->_szIp, sock->_port, user._user_id.c_str(), "CPccServer Disconnection fd %d" , sock->_fd );
	_online_user.DeleteUser( sock );
}

// 心跳线程和定时线程
void CPccServer::TimeWork()
{
	//做超时检测使用
	while (1){
		if ( ! Check() )  break ;
		// 处理超时的下行连接
		HandleOfflineUsers();

		sleep(1);
	}
}

void CPccServer::NoopWork()
{
}

// 发送关闭连接请求, msgid:UP_DISCONNECT_INFORM,UP_CLOSELINK_INFORM
void CPccServer::Close( int accesscode , unsigned short msgid, int reason )
{
	OUT_PRINT( NULL, 0, NULL, "close accesscode %d down link, msgid %d, reason %d" , accesscode, msgid, reason ) ;

	char uid[128] = {0};
	sprintf( uid, "%d", accesscode ) ;

	User user = _online_user.GetUserByUserId( uid ) ;
	if ( user._user_id.empty() ) {
		return ;
	}

	switch( msgid ) {
	case UP_CLOSELINK_INFORM:  // 关闭主从链路
	case UP_DISCONNECT_INFORM: // 关闭主链路
		{
			UpDisconnectInform req ;
			req.header.msg_len 		= ntouv32( sizeof(req) ) ;
			req.header.msg_seq 		= ntouv32( _proto_parse.get_next_seq() ) ;
			req.header.access_code  = ntouv32( accesscode ) ;
			req.header.msg_type		= ntouv16( msgid ) ;
			req.error_code			= reason ;
			// 发送断连数据
			SendCrcData( user._fd, (const char *)&req, sizeof(req) ) ;
			/**
			if ( msgid == UP_CLOSELINK_INFORM ) {
				// 关闭主链路请求处理
				_pEnv->GetPasClient()->Close( accesscode ) ;
			}*/
			OUT_INFO( user._ip.c_str(), user._port, user._user_id.c_str(), "Send %s",
					(msgid == UP_CLOSELINK_INFORM) ? "UP_CLOSELINK_INFORM" : "UP_DISCONNECT_INFORM" ) ;
		}
		break ;
	default:  // 断开连接处理
		{
			user._user_state = User::OFF_LINE ;
			_online_user.SetUser( uid, user ) ;

			OUT_INFO( user._ip.c_str(), user._port, user._user_id.c_str(), "Close Sublink set user state offline" ) ;
		}
		break ;
	}
}

void CPccServer::HandleOfflineUsers()
{
	vector<User> vec_users = _online_user.GetOfflineUsers(MAX_TIMEOUT);

	// 处理用户断连请求
	for(int i = 0; i < (int)vec_users.size(); ++i){
		User &user = vec_users[i];
		if(user._socket_type == User::TcpClient){
			if(user._fd != NULL ){
				OUT_WARNING( user._ip.c_str() , user._port , user._user_id.c_str() ,
						"CPccServer close socket fd %d", user._fd->_fd );
				CloseSocket(user._fd);
			}
		}
	}
}

bool CPccServer::SendCrcData( socket_t *sock, const char* data, int len )
{
	if ( sock == NULL )
		return false ;

	// 处理循环码
	char *buf = new char[len+1] ;
	memset( buf, 0 , len+1 ) ;
	memcpy( buf, data, len ) ;

	// 处理加解密数据
	EncryptData( (unsigned char*) buf , len , true ) ;

	// 统一附加循环码的验证
	unsigned short crc_code = ntouv16( GetCrcCode( buf, len ) ) ;
	unsigned int   offset   = len - sizeof(Footer) ;
	// 替换循环码内存的位置数据
	memcpy( buf + offset , &crc_code, sizeof(short) ) ;

	C5BCoder coder ;
	coder.Encode( buf , len ) ;

	delete [] buf ;

	return SendData( sock, coder.GetData(), coder.GetSize() ) ;
}

// 加密处理数据
bool CPccServer::EncryptData( unsigned char *data, unsigned int len , bool encode )
{
	if ( len < sizeof(Header) )
		return false ;

	Header *header = ( Header *) data ;
	// 是否需要加密处理
	if ( ! header->encrypt_flag && ! encode ) {
		return false;
	}

	int M1 = 0, IA1 = 0 , IC1 = 0 ;
	int accesscode = ntouv32( header->access_code ) ;
	// 密钥是否为空如果为空不需要处理
	if ( ! _pEnv->GetUserKey(accesscode, M1, IA1, IC1 ) ) {
		return false ;
	}

	// 如果为加密处理
	if ( encode ) {
		// 设置加密标志位
		header->encrypt_flag =  1 ;
		// 添加加密密钥
		header->encrypt_key  =  ntouv32( CEncrypt::rand_key() ) ;
	}

	// 解密数据
	return CEncrypt::encrypt( M1, IA1, IC1, (unsigned char *)data, (unsigned int) len ) ;
}
