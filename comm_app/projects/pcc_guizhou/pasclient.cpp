#include "pasclient.h"
#include <ProtoHeader.h>
#include <Base64.h>
#include "pccutil.h"
#include <crc16.h>
#include <comlog.h>
#include <tools.h>
#include "pconvert.h"
#include <BaseTools.h>

PasClient::PasClient(): _macid2seqid(true), _statinfo("PasClient")
{
	_last_handle_user_time = time(NULL) ;
	_down_port = 0 ;
}

PasClient::~PasClient()
{
	Stop() ;
}

bool PasClient::Init( ISystemEnv *pEnv )
{
	_pEnv = pEnv ;

	char value[1024] = {0} ;
	if ( _pEnv->GetString("pcc_down_ip", value) ){
		_down_ip = value ;
	}
	// 取得自动应答的平台查岗的数据
	if ( _pEnv->GetString( "pcc_postquery", value ) ) {
		_postpath = value ;
	}

	int port = 0 ;
	if ( _pEnv->GetInteger("pcc_listen_port", port ) ){
		_down_port = port ;
	}

	// 设置用户处理回调对象
	_pEnv->SetNotify( PAS_USER_TAG , this ) ;

	return true ;
}

void PasClient::Stop( void )
{
	StopClient() ;
}

bool PasClient::Start( void )
{
	return StartClient( "0.0.0.0", 0, 3 ) ;
}

bool PasClient::ConnectServer(User &user, int timeout /*= 10*/)
{
	if(time(0) - user._connect_info.last_reconnect_time < user._connect_info.timeval)
			return false;

	bool ret = false;
	if (user._fd  != NULL)
	{
		OUT_WARNING( user._ip.c_str(), user._port ,NULL,"fd %d close socket", user._fd->_fd );
		CloseSocket(user._fd);
	}
	user._fd = _tcp_handle.connect_nonb(user._ip.c_str(), user._port, timeout);
	ret = (user._fd != NULL) ? true:false;

	user._last_active_time = time(0);
	user._login_time       = time(0);
	user._connect_info.last_reconnect_time = time(0);

	if(ret )
	{
		user._user_state = User::WAITING_RESP;

		// 先添加后发送数据
		_online_user.AddUser( user._user_id, user ) ;

		UpConnectReq req;
		req.header.msg_seq 		= ntouv32(_proto_parse.get_next_seq());
		req.header.access_code  = ntouv32( user._access_code);
		req.user_id        		= ntouv32( atoi(user._user_name.c_str()) ) ;
		memcpy( req.password , user._user_pwd.c_str(), sizeof(req.password) ) ;

		// 如果设置下行就需要处理下行
		if ( _down_port > 0 ) {
			safe_memncpy( (char*)req.down_link_ip, _down_ip.c_str(), sizeof(req.down_link_ip) ) ;
			req.down_link_port = ntouv16( _down_port ) ;
		}
		OUT_INFO(user._ip.c_str(),user._port,user._user_id.c_str(),"Send UpConnectReq,down-link state:CONNECT_WAITING_RESP");
		SendCrcData( user._fd , (const char*)&req, sizeof(req) );
	}
	else
	{
		user._user_state = User::OFF_LINE;
	}

	/**
	if(user._connect_info.keep_alive == ReConnTimes)
		user._connect_info.reconnect_times--;
	*/
	return ret;
}

void PasClient::on_data_arrived( socket_t *sock, const void* data, int len)
{
	C5BCoder coder ;
	if ( ! coder.Decode( (const char *)data, len ) )
	{
		OUT_WARNING(sock->_szIp, sock->_port, NULL,"Except packet header or tail");
		return;
	}

	// 处理加解密数据
	EncryptData( (unsigned char*) coder.GetData() , (unsigned int)coder.GetSize() , false ) ;
	// 发送数据
	HandleOnePacket( sock,(const char*)coder.GetData() , coder.GetSize() );
}

// 处理收着的来自省平台DOWN的数据
void PasClient::HandlePasDownData( const int access, const char *data, int len )
{
	User user = _online_user.GetUserByAccessCode( access ) ;
	if ( user._user_id.empty() || user._user_state != User::ON_LINE ) {
		OUT_WARNING( user._ip.c_str(), user._port, user._user_id.c_str(), "HandlePasDownData user not online" ) ;
		return ;
	}
	// 处理从下行链路过来数据
	HandleOnePacket( user._fd , data , len ) ;
}

void PasClient::on_dis_connection( socket_t *sock )
{
	//专门处理底层的链路突然断开的情况，不处理超时和正常流程下的断开情况。
	User user = _online_user.GetUserBySocket( sock );
	if ( ! user._user_id.empty() || user._fd == NULL ) {
		// 处理断连状态更新
		//_srvCaller.updateConnectState( UP_DISCONNECT_RSP , _pEnv->GetSequeue() , GetAreaCode(user) , CONN_MASTER , CONN_DISCONN ) ;
	}
	if ( user._user_state != User::DISABLED ) {
		OUT_WARNING( sock->_szIp, sock->_port, user._user_id.c_str(), "Disconnection fd %d", sock->_fd );
		user._user_state = User::OFF_LINE ;
	}
	user._fd = NULL ;
	_online_user.SetUser( user._user_id, user ) ;
}

void PasClient::TimeWork()
{
	/*
	 * 1.将超时的连接去掉；
	 * 2.定时发送NOOP消息
	 * 3.Reload配置文件中的新的连接。
	 * 4.
	 */
	while(1) {
		if ( ! Check() ) break ;
		// 处理连接
		HandleOfflineUsers() ;

		// 设置缓存十分钟超时
		_pEnv->GetMsgCache()->CheckData( 600 ) ;
		// 五分钟超时时间
		_macid2seqid.CheckTimeOut( 300 ) ;

		sleep(2) ;
	}
}

void PasClient::NoopWork()
{
	while(1)
	{
		// 针对在线用户发送心跳包
		HandleOnlineUsers( 30 ) ;
		// 打印统计信息处理
		_statinfo.Check() ;

		sleep(3) ;
	}
}

// 向PAS交数据通过接入码
void PasClient::HandlePasUpData( const int access, const char *data, int len )
{
	User user = _online_user.GetUserByAccessCode( access ) ;
	if ( user._user_id.empty() ) {
		OUT_WARNING( user._ip.c_str(), user._port, user._user_id.c_str(), "HandlePasDownData user empty" ) ;
		return ;
	}

	// 用户没有在线的情况
	if ( user._user_state != User::ON_LINE ) {
		OUT_WARNING( user._ip.c_str(), user._port, user._user_id.c_str(), "HandlePasDownData user not online" ) ;
		return ;
	}

	Header *header = ( Header *) data ;
	header->access_code = ntouv32( user._access_code ) ;
	// 发送数据重新添加循环码的处理
	if ( ! SendCrcData( user._fd, data, len ) ) {
		// Todo: Send failed
	}
}

void PasClient::HandleClientData( const char *code, const char *data, int len )
{
	if ( SendDataToUser( code, data, len ) ) {
		OUT_SEND( NULL, 0, code, "Send data %s", _proto_parse.Decoder(data, len).c_str() ) ;
	} else {
		OUT_ERROR( NULL, 0, code, "Send Data %s Failed", _proto_parse.Decoder(data, len).c_str() ) ;
	}
}

bool PasClient::SendDataToUser( const string &area_code, const char *data, int len)
{
	char buf[512] = {0};
	sprintf( buf, "%s%s", PAS_USER_TAG, area_code.c_str() ) ;

	User user = _online_user.GetUserByUserId(buf);
	if (user._user_id.empty()) {
		return false;
	}

	// 用户没有在线的情况
	if ( user._user_state != User::ON_LINE ) {
		OUT_WARNING( user._ip.c_str(), user._port, buf, "HandlePasDownData user not online" ) ;
		return false;
	}

	Header *header = ( Header *) data ;
	header->access_code = ntouv32( user._access_code ) ;

	// 如果为扩展消息则添加车辆统计中处理
	if ( ntouv16(header->msg_type) == UP_EXG_MSG ) {
		char szmacid[128] = {0} ;
		ExgMsgHeader *msgheader = (ExgMsgHeader*) (data + sizeof(Header));
		sprintf( szmacid, "%d_%s", msgheader->vehicle_color, (const char *) msgheader->vehicle_no ) ;
		_statinfo.AddVechile( user._access_code, szmacid, STAT_SEND ) ;
	}

	// 发送数据重新添加循环码的处理
	return SendCrcData( user._fd, data, len ) ;
}

void PasClient::HandleOfflineUsers()
{
	vector<User> vec_users = _online_user.GetOfflineUsers(3*60);
	for(int i = 0; i < (int)vec_users.size(); i++) {
		User &user = vec_users[i];
		if(user._socket_type == User::TcpClient){
			if(user._fd != NULL){
				OUT_WARNING( user._ip.c_str() , user._port , user._user_id.c_str() ,
						"HandleOfflineUsers PasClient TcpClient close socket fd %d", user._fd->_fd );
				CloseSocket(user._fd);
			}
		} else if(user._socket_type == User::TcpConnClient) {
			if(user._fd !=NULL ){
				OUT_INFO( user._ip.c_str() , user._port , user._user_id.c_str() ,
						"HandleOfflineUsers PasClient TcpConnClient close socket fd %d", user._fd->_fd );
				user.show();
				CloseSocket(user._fd);
				user._fd = NULL;
			}
			if ( ! ConnectServer(user, 10) ) {
				//连接失败，一样需要处理。
				_online_user.AddUser( user._user_id, user ) ;
			}
		}
	}
}

void PasClient::HandleOnlineUsers(int timeval)
{
	time_t now = time(NULL) ;
	if( now - _last_handle_user_time < timeval){
		return;
	}
	_last_handle_user_time = now ;

	vector<User> vec_users = _online_user.GetOnlineUsers();
	for(int i = 0; i < (int)vec_users.size(); i++)
	{
		User &user = vec_users[i] ;
		if( user._socket_type == User::TcpConnClient && user._fd != NULL ) {
			UpLinkTestReq req;
			req.header.access_code = ntouv32(user._access_code);
			req.header.msg_seq 	   = ntouv32(_proto_parse.get_next_seq());
			req.crc_code 		   = ntouv16(GetCrcCode((const char*)&req,sizeof(req)));
			Send5BCodeData( user._fd,(const char*)&req,sizeof(req), false );

			OUT_SEND( user._ip.c_str(), user._port, user._user_id.c_str(),
					"%s", _proto_parse.Decoder((const char*)&req,sizeof(req)).c_str());
		}
	}
}

// 发送数据进行5B编码处理
bool PasClient::Send5BCodeData( socket_t *sock, const char *data, int len  , bool bflush )
{
	if ( sock == NULL )  {
		return false ;
	}
	C5BCoder  coder;
	if ( ! coder.Encode( data, len ) ){
		OUT_ERROR( sock->_szIp, sock->_port, NULL, "Send5BCodeData failed , socket fd %d", sock->_fd ) ;
		return false ;
	}

	OUT_HEX(sock->_szIp, sock->_port, "SEND", coder.GetData(), coder.GetSize());

	return SendData( sock, coder.GetData(), coder.GetSize() ) ;
}

// 发送重新处理循环码的数据
bool PasClient::SendCrcData( socket_t *sock, const char* data, int len)
{
	// 处理循环码
	char *buf = new char[len+1] ;
	memcpy( buf, data, len ) ;
	// 处理加解密数据
	EncryptData( (unsigned char*) buf , len , true ) ;
	// 统一附加循环码的验证
	unsigned short crc_code = ntouv16( GetCrcCode( buf, len ) ) ;
	unsigned int   offset   = len - sizeof(Footer) ;
	// 替换循环码内存的位置数据
	memcpy( buf + offset , &crc_code, sizeof(short) ) ;

	bool bSend = Send5BCodeData( sock, buf , len ) ;

	delete [] buf ;

	return bSend ;
}

void PasClient::HandleOnePacket( socket_t *sock, const char* data , int len )
{
	const char *ip = sock->_szIp ;
	unsigned short port = sock->_port ;

	if(  len < (int)sizeof(Header) || data == NULL ){
		OUT_ERROR( ip, port, NULL, "data length errro length %d", len ) ;
		OUT_HEX( ip, port, NULL , data, len ) ;
		return ;
	}

	Header *header = (Header *) data;
	unsigned int access_code = ntouv32(header->access_code);
	string str_access_code   = uitodecstr(access_code);
	unsigned int msg_len     = ntouv32(header->msg_len);
	unsigned short msg_type  = ntouv16(header->msg_type);
	string mac_id = _proto_parse.GetMacId( data , len );

	// 添加接收到的数据统计服务
	_statinfo.AddRecv( access_code ) ;

	if ( msg_len > len || msg_len == 0 ) {
		OUT_ERROR( ip, port, NULL, "data length errro length %d , msg len %d", len , msg_len ) ;
		OUT_HEX( ip, port, NULL , data, len ) ;
		return ;
	}

	OUT_RECV( ip, port, str_access_code.c_str(), "%s", _proto_parse.Decoder(data,len).c_str() ) ;
	OUT_HEX( ip, port, str_access_code.c_str(), data, len ) ;

	User user = _online_user.GetUserBySocket( sock ) ;

	if (msg_type == UP_CONNECT_RSP)
	{
		UpConnectRsp *rsp = ( UpConnectRsp *) data ;
		switch(rsp->result)
		{
		case 0:
			OUT_INFO(ip,port,str_access_code.c_str(),"login check success,access_code:%d  up-link ON_LINE",access_code);
			user._user_state  = User::ON_LINE ;
			break;
		case 1:
			OUT_WARNING(ip,port,str_access_code.c_str(),"login check fail,ip is invalid");
			break;
		case 2:
			OUT_WARNING(ip,port,str_access_code.c_str(),"login check fail,accesscode is invalid,close it");
			break;
		case 3:
			OUT_WARNING(ip,port,str_access_code.c_str(),"login check fail,user_name:%s is invalid,close it", user._user_name.c_str());
			break;
		case 4:
			OUT_WARNING(ip,port,str_access_code.c_str(),"login check fail,user_password:%s is invalid,close it",user._user_pwd.c_str());
			break;
		default:
			OUT_WARNING(ip,port,str_access_code.c_str(),"login check fail,other error,close it");
			break;
		}

		if ( rsp->result != 0 ) {
			CloseSocket( sock ) ;
			return ;
		}

		// 处理激活
		user._last_active_time = time(NULL) ;
		// 设置用户将态
		_online_user.SetUser( user._user_id, user ) ;
		// 处理连接状态更新
		//_srvCaller.updateConnectState( UP_CONNECT_RSP , _pEnv->GetSequeue() , areacode, CONN_MASTER , CONN_CONNECT ) ;

		return ;
	}
	else if (msg_type == UP_LINKTEST_RSP)//"NOOP_ACK")
	{
	}
	else if ( msg_type == UP_DISCONNECT_RSP ) // 收到断开连接响应则直接处理离线状态
	{
		// 主动申请断连操作，收到允许断连响应后再断开连接
		if ( User::DISABLED == user._user_state ) {
			if ( user._fd != NULL ) {
				OUT_ERROR( ip, port, str_access_code.c_str(), "disconnect response fd %d" , user._fd->_fd ) ;
				// 关闭连接
				CloseSocket( user._fd ) ;
			}
		} else {
			user._user_state = User::OFF_LINE ;
			// 发送关闭从链路请求，这里是非异常情况处理，这里是正常注销
			_pEnv->GetPccServer()->Close( access_code , UP_DISCONNECT_INFORM , 0x00 ) ;
		}
	}
	else if ( msg_type == DOWN_DISCONNECT_INFORM )   // 主链路下发从链路断开通知
	{
		// 主动关闭从链路连接
		// _pEnv->GetPccServer()->Close( user._access_code, 0, 0 ) ;
		OUT_INFO( ip, port, str_access_code.c_str(), "Recv DOWN_DISCONNECT_INFORM" ) ;
	}
	else if ( msg_type == DOWN_CLOSELINK_INFORM )  // 主动关闭主从链路通知
	{
		// 主动关闭主从链路连接
		// _pEnv->GetPccServer()->Close( user._access_code, 0, 0 ) ;
		OUT_INFO( ip, port, str_access_code.c_str(), "Recv DOWN_CLOSELINK_INFORM" ) ;
	}
	else if ( msg_type == DOWN_PLATFORM_MSG )
	{
		DownPlatformMsg *plat_msg = ( DownPlatformMsg * ) ( data + sizeof(Header) ) ;
		unsigned short data_type = ntouv16( plat_msg->data_type ) ;
		switch( data_type ) {
		case DOWN_PLATFORM_MSG_POST_QUERY_REQ:  // 平台查岗消息
			{
				if ( len < (int)sizeof(DownPlatformMsgPostQueryReq) ) {
					OUT_ERROR( ip,port,str_access_code.c_str(), "DOWN_PLATFORM_MSG_POST_QUERY_REQ data length error , length %d" , len ) ;
					return ;
				}
				DownPlatformMsgPostQueryReq *msg = (DownPlatformMsgPostQueryReq*)data;

				// 取得平台查岗的长度
				int nlen = ntouv32( msg->down_platform_body.info_length ) ;
				if ( nlen < 0 || nlen + (int)sizeof(DownPlatformMsgPostQueryReq) > len ) {
					OUT_ERROR( ip,port,str_access_code.c_str(), "DOWN_PLATFORM_MSG_POST_QUERY_REQ data length error , length %d, content len %d" , len , nlen ) ;
					return ;
				}

				UpPlatformMsgPostQueryAck resp;

				resp.header.msg_len               = 0 ;  // 暂时取0，等收到响应后再重新计算
				resp.header.access_code 	      = ntouv32( user._access_code ) ;
				resp.header.msg_seq 		      = msg->header.msg_seq ;
				resp.up_platform_msg.data_length  = 0 ;
				resp.up_platform_post.info_id	  = msg->down_platform_body.info_id ;
				resp.up_platform_post.msg_len	  = 0 ;
				resp.up_platform_post.object_type = msg->down_platform_body.object_type ;
				safe_memncpy( resp.up_platform_post.object_id, msg->down_platform_body.object_id , sizeof(resp.up_platform_post.object_id) ) ;

				std::string text ;

				CQString content;
				content.SetString((const char*) (data + sizeof(DownPlatformMsgPostQueryReq)), nlen);

				CBase64 base64;
				base64.Encode(content.GetBuffer(), content.GetLength());

				unsigned int seqid = _pEnv->GetSequeue();
				char szKey[256] = { 0 };
				_pEnv->GetCacheKey(seqid, szKey);

				// 添加到等待缓存中
				_pEnv->GetMsgCache()->AddData(szKey, (const char *) &resp, sizeof(resp));

				char messageId[512] = { 0 };
				sprintf(messageId, "%u", ntouv32( msg->down_platform_body.info_id ));

				char objectType[128] = { 0 };
				sprintf(objectType, "%u", msg->down_platform_body.object_type);

				char objectId[256] = { 0 };
				safe_memncpy(objectId, msg->down_platform_body.object_id, sizeof(msg->down_platform_body.object_id));

				char areaId[128] = { 0 };
				sprintf(areaId, "%u", GetAreaCode(user));

				char macid[64];
				snprintf(macid, 64, "%u_%s", user._access_code, areaId);

				//CAITS 1_2_3 11000020_430000 4 L_PLAT {TYPE:D_PLAT,PLATQUERY:2|500105010649|147258369|MSszPT8=}

				string inner = "CAITS " + string(szKey) + " " + string(macid) + " 4 L_PLAT {TYPE:D_PLAT,PLATQUERY";
				inner += ":" + string(objectType);
				inner += "|" + string(objectId);
				inner += "|" + string(messageId);
				inner += "|" + string(base64.GetBuffer());
				inner += "} \r\n";

				// 处理平台查岗
				//_srvCaller.addForMsgPost( DOWN_PLATFORM_MSG_POST_QUERY_REQ , seqid, content.GetBuffer() , messageId , objectId, objectType , areaId ) ;
				((IMsgClient*) _pEnv->GetMsgClient())->HandleUpMsgData(SEND_ALL, inner.c_str(), inner.length());

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_PLATFORM_MSG_POST_QUERY_REQ seqid %u, messageId %s, objectId %s, objectType %s, areaId %s, content %s, key: %s" ,
						seqid , messageId, objectId, objectType, areaId , content.GetBuffer(), szKey );
			}
			break ;
		case DOWN_PLATFORM_MSG_INFO_REQ:
			{
				if ( len < (int)sizeof(DownPlatformMsgInfoReq) ) {
					OUT_ERROR( ip,port,str_access_code.c_str(), "DOWN_PLATFORM_MSG_INFO_REQ data length error , length %d" , len ) ;
					return ;
				}
				//平台下发报文请求
				DownPlatformMsgInfoReq * msg = ( DownPlatformMsgInfoReq *) data ;

				// 取得平台查岗的长度
				int nlen = ntouv32( msg->info_length ) ;
				if ( nlen < 0 || nlen + (int)sizeof(DownPlatformMsgInfoReq) > len ) {
					OUT_ERROR( ip,port,str_access_code.c_str(), "DOWN_PLATFORM_MSG_INFO_REQ data length error , length %d, content len %d" , len , nlen ) ;
					return ;
				}

				CQString content ;
				content.SetString( (const char*)(data+sizeof(DownPlatformMsgInfoReq)) , nlen ) ;

				CBase64 base64;
				base64.Encode(content.GetBuffer(), content.GetLength());

				unsigned int seqid = _pEnv->GetSequeue() ;
				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				UpPlatFormMsgInfoAck resp;

				resp.header.msg_len               = ntouv32( sizeof(resp) ) ;
				resp.header.msg_type			  = ntouv16( UP_PLATFORM_MSG ) ;
				resp.header.access_code 	      = ntouv32( user._access_code ) ;
				resp.header.msg_seq 		      = msg->header.msg_seq ;
				resp.up_platform_msg.data_type	  = ntouv16( UP_PLATFORM_MSG_INFO_ACK ) ;
				resp.up_platform_msg.data_length  = ntouv32( sizeof(int) ) ;
				resp.info_id 					  = msg->info_id ;

				// 添加到等待缓存中
				//_pEnv->GetMsgCache()->AddData( szKey, (const char *)&resp, sizeof(resp) ) ;

				char messageId[512] = {0} ;
				sprintf( messageId, "%d" , ntouv32(  msg->info_id ) ) ;

				char objectType[128] = {0} ;
				sprintf( objectType, "%d", msg->object_type ) ;

				char objectId[256] = {0} ;
				safe_memncpy( objectId, msg->object_id , sizeof(msg->object_id) ) ;

				char areaId[128] = {0} ;
				sprintf( areaId, "%u" , GetAreaCode(user) ) ;

				char macid[64];
				snprintf(macid, 64, "%u_%s", user._access_code, areaId);

				string inner = "CAITS " + string(szKey) + " " + string(macid) + " 4 L_PLAT {TYPE:D_PLAT,PLATMSG";
				inner += ":" + string(objectType);
				inner += "|" + string(objectId);
				inner += "|" + string(messageId);
				inner += "|" + string(base64.GetBuffer());
				inner += "} \r\n";

				// 处理平台查岗
				//_srvCaller.addForMsgInfo( DOWN_PLATFORM_MSG_INFO_REQ , seqid, content.GetBuffer() , messageId , objectId, objectType , areaId ) ;
				((IMsgClient*)_pEnv->GetMsgClient())->HandleUpMsgData(SEND_ALL, inner.c_str(), inner.length() ) ;

				HandlePasUpData(access_code, (char*)&resp, sizeof(UpPlatFormMsgInfoAck));

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_PLATFORM_MSG_INFO_REQ seqid %u, messageId %s, objectId %s, objectType %s, areaId %s, content %s" ,
										seqid , messageId, objectId, objectType, areaId , content.GetBuffer() ) ;
			}
			break ;
		}
	}
	else if ( msg_type == DOWN_CTRL_MSG )
	{
		if ( len < (int)sizeof(DownCtrlMsgHeader) ){
			OUT_ERROR( ip,port,str_access_code.c_str(), "DOWN_CTRL_MSG data length error , length %d" , len ) ;
			return ;
		}

		DownCtrlMsgHeader *req = ( DownCtrlMsgHeader *) data ;
		int data_type = ntouv16( req->ctrl_msg_header.data_type ) ;

		char carnum[128]= {0};
		safe_memncpy( carnum, req->ctrl_msg_header.vehicle_no, sizeof(req->ctrl_msg_header.vehicle_no) ) ;
		char carcolor[128] = {0} ;
		sprintf( carcolor, "%d" , req->ctrl_msg_header.vehicle_color ) ;

		char macidSrc[32];
		snprintf(macidSrc, 32, "%s_%s", carcolor, carnum);

		string macidDst = "";
		if( ! _pEnv->GetRedisCache()->HGet("KCTX.PLATE2SIM", macidSrc, macidDst) || macidDst.empty()) {
			OUT_WARNING(user._ip.c_str(), user._port, user._user_name.c_str(), "KCTX.PLATE2SIM: %s not exist", macidSrc);
			return;
		}

		switch( data_type )
		{
		case DOWN_CTRL_MSG_MONITOR_VEHICLE_REQ: //从链路 处理监控
			{
				if ( len < (int)sizeof(DownCtrlMsgMonitorVehicleReq) ){
					OUT_ERROR( ip,port,str_access_code.c_str(), "DOWN_CTRL_MSG_MONITOR_VEHICLE_REQ data length %d error" , len ) ;
					return ;
				}

				DownCtrlMsgMonitorVehicleReq *msg = (DownCtrlMsgMonitorVehicleReq *) data;

				unsigned int seqid = _pEnv->GetSequeue() ;

				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				char szphone[128] = {0};
				safenumber( szphone, msg->monitor_tel, sizeof(msg->monitor_tel) ) ;

				// 将数据缓存，然后要根据缓存数据对应起来
				UpCtrlMsgMonitorVehicleAck resp;//ack 应该通过主链路回复
				resp.header.msg_len 	= ntouv32(sizeof(UpCtrlMsgMonitorVehicleAck));
				resp.header.access_code = ntouv32( user._access_code ) ;
				resp.header.msg_seq 	= msg->header.msg_seq ;
				resp.ctrl_msg_header.vehicle_color = msg->ctrl_msg_header.vehicle_color;
				safe_memncpy(resp.ctrl_msg_header.vehicle_no, msg->ctrl_msg_header.vehicle_no, sizeof(resp.ctrl_msg_header.vehicle_no) );
				resp.ctrl_msg_header.data_length = ntouv32( sizeof(unsigned char) ) ;
				resp.result 			= 0x00;

				// 添加到缓存队列中
				_pEnv->GetMsgCache()->AddData( szKey, (const char *)&resp, sizeof(resp) ) ;

				int szlen = 0;
				char szbuf[1024] = {0} ;
				szlen += snprintf(szbuf + szlen, 1024 - szlen, "CAITS %s %s", szKey, macidDst.c_str());
				szlen += snprintf(szbuf + szlen, 1024 - szlen, " 4 D_CTLM {TYPE:9,RETRY:0,VALUE:%s} \r\n" , szphone ) ;

				((IMsgClient*)_pEnv->GetMsgClient())->HandleUpMsgData( macidDst.c_str(), szbuf, szlen) ;
				// 调用HTTP处理数据
				//_srvCaller.getTernimalByVehicleByTypeEx( DOWN_CTRL_MSG_MONITOR_VEHICLE_REQ, seqid, carnum , carcolor , szbuf ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_CTRL_MSG_MONITOR_VEHICLE_REQ seqid %u, carnum %s, carcolor %s" , seqid , carnum, carcolor ) ;
			}
			break;
		case DOWN_CTRL_MSG_TAKE_PHOTO_REQ:  // 处理拍照
			{
				if ( len < (int)sizeof(DownCtrlMsgTakePhotoReq) ) {
					OUT_ERROR( ip,port,str_access_code.c_str(), "DOWN_CTRL_MSG_TAKE_PHOTO_REQ data length %d error" , len ) ;
					return ;
				}

				DownCtrlMsgTakePhotoReq *msg = ( DownCtrlMsgTakePhotoReq *) data ;

				char szKey[256]={0};
				unsigned int seqid = _pEnv->GetSequeue() ;
				_pEnv->GetCacheKey( seqid, szKey ) ;

				// 摄像头通道ID|拍摄命令|录像时间|保存标志|分辨率|照片质量|亮度|对比度|饱和度|色度
				int szlen = 0;
				char szbuf[1024] = {0} ;
				szlen += snprintf(szbuf + szlen, 1024 - szlen, "CAITS %s %s", szKey, macidDst.c_str());
				szlen += snprintf(szbuf + szlen, 1024 - szlen, " 4 D_CTLM {TYPE:10,RETRY:0,VALUE:"
						"%d|1|1|0|%d|10|128|128|128|128} \r\n", msg->lens_id, msg->size - 1);

				((IMsgClient*)_pEnv->GetMsgClient())->HandleUpMsgData( macidDst.c_str(), szbuf, szlen) ;

				//_srvCaller.getTernimalByVehicleByTypeEx( DOWN_CTRL_MSG_TAKE_PHOTO_REQ, seqid, carnum , carcolor, szbuf ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_CTRL_MSG_TAKE_PHOTO_REQ seqid %u, carnum %s, carcolor %s" , seqid , carnum, carcolor ) ;
			}
			break ;
		case DOWN_CTRL_MSG_TEXT_INFO:  // 下发文本
			{
				if ( len < (int)sizeof(DownCtrlMsgTextInfoHeader) ) {
					OUT_ERROR( ip, port, str_access_code.c_str(), "DOWN_CTRL_MSG_TEXT_INFO data length %d error" , len ) ;
					return  ;
				}

				DownCtrlMsgTextInfoHeader *msg = ( DownCtrlMsgTextInfoHeader *)data ;

				unsigned int seqid = _pEnv->GetSequeue() ;
				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				CBase64 base;
				int nlen = ntouv32( msg->msg_len ) ;
				base.Encode( data + sizeof(DownCtrlMsgTextInfoHeader) , nlen ) ;

				UpCtrlMsgTextInfoAck resp ;
				resp.header.msg_len 	= ntouv32(sizeof(UpCtrlMsgTextInfoAck));
				resp.header.access_code = ntouv32( user._access_code ) ;
				resp.header.msg_seq 	= msg->header.msg_seq ;
				resp.ctrl_msg_header.vehicle_color = msg->ctrl_msg_header.vehicle_color;
				safe_memncpy(resp.ctrl_msg_header.vehicle_no, msg->ctrl_msg_header.vehicle_no, sizeof(resp.ctrl_msg_header.vehicle_no) );
				resp.ctrl_msg_header.data_length = ntouv32( sizeof(unsigned char) + sizeof(int) ) ;
				resp.msg_id				= msg->msg_sequence ;
				resp.result 			= 0x00;

				//添加到缓存队列中
				_pEnv->GetMsgCache()->AddData( szKey , (const char *)&resp, sizeof(resp));
				OUT_INFO( ip, port, str_access_code.c_str() , "Add UpCtrlmsgTexInfoAck key %s", szKey ) ;

				int szlen = 0;
				char szbuf[1024] = {0} ;
				szlen += snprintf(szbuf + szlen, 1024 - szlen, "CAITS %s %s", szKey, macidDst.c_str());
				szlen += snprintf(szbuf + szlen, 1024 - szlen, " 4 D_SNDM {TYPE:1,1:255,2:%s} \r\n",  base.GetBuffer());

				((IMsgClient*)_pEnv->GetMsgClient())->HandleUpMsgData( macidDst.c_str(), szbuf, szlen) ;

				//_srvCaller.getTernimalByVehicleByTypeEx( DOWN_CTRL_MSG_TEXT_INFO, seqid, carnum , carcolor, inner.c_str() ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_CTRL_MSG_TEXT_INFO seqid %u, carnum %s, carcolor %s" , seqid , carnum, carcolor ) ;
			}
			break ;
		case DOWN_CTRL_MSG_TAKE_TRAVEL_REQ:  //2011-11-29 xfm 上报车辆行驶记录请求
			{
				if ( len < (int)sizeof(DownCtrlMsgTaketravelReq) ){
					OUT_ERROR( ip, port, str_access_code.c_str(), "DOWN_CTRL_MSG_TAKE_TRAVEL_REQ data length %d error" , len ) ;
					return ;
				}

				DownCtrlMsgTaketravelReq *msg = (DownCtrlMsgTaketravelReq *)data;

				unsigned int seqid = _pEnv->GetSequeue() ;

				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				UpCtrlMsgTaketravel resp;

				resp.header.msg_len 	= ntouv32(sizeof(UpCtrlMsgTaketravel));
				resp.header.access_code = ntouv32( user._access_code ) ;
				resp.header.msg_seq 	= msg->header.msg_seq;
				resp.header.msg_type    = ntouv16( UP_CTRL_MSG ) ;
				resp.command_type       = msg->command_type;
				resp.ctrl_msg_header.data_type     = ntouv16( UP_CTRL_MSG_TAKE_TRAVEL_ACK ) ;
				resp.ctrl_msg_header.vehicle_color = msg->ctrl_msg_header.vehicle_color;
				safe_memncpy(resp.ctrl_msg_header.vehicle_no, msg->ctrl_msg_header.vehicle_no, sizeof(resp.ctrl_msg_header.vehicle_no) );

				resp.ctrl_msg_header.data_length = 0 ;

				//添加到缓存队列中
				_pEnv->GetMsgCache()->AddData( szKey, (const char *)&resp, sizeof(resp));
				OUT_INFO( ip, port, str_access_code.c_str() , "Add UpCtrlMsgTaketravelAck key %s", szKey );

				int szlen = 0;
				char szbuf[1024] = {0} ;
				szlen += snprintf(szbuf + szlen, 1024 - szlen, "CAITS %s %s", szKey, macidDst.c_str());
				szlen += snprintf(szbuf + szlen, 1024 - szlen, " 4 D_REQD {TYPE:4,30:%u} \r\n",  msg->command_type);

				((IMsgClient*)_pEnv->GetMsgClient())->HandleUpMsgData( macidDst.c_str(), szbuf, szlen) ;
				//_srvCaller.getTernimalByVehicleByTypeEx( DOWN_CTRL_MSG_TAKE_TRAVEL_REQ, seqid, carnum , carcolor, szbuf ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_CTRL_MSG_TAKE_TRAVEL_REQ seqid %u, carnum %s, carcolor %s" , seqid , carnum, carcolor ) ;
			}
			break;
		case DOWN_CTRL_MSG_EMERGENCY_MONITORING_REQ:  //2011-11-29 xfm 紧急接入监管平台
		    {
		    	if ( len < (int)sizeof(DownCtrlMsgEmergencyMonitoringReq) ){
					OUT_ERROR( ip, port, str_access_code.c_str(), "DOWN_CTRL_MSG_EMERGENCY_MONITORING_REQ data length %d error" , len ) ;
					return ;
				}

				DownCtrlMsgEmergencyMonitoringReq *msg = (DownCtrlMsgEmergencyMonitoringReq *)data;

				unsigned int seqid = _pEnv->GetSequeue() ;
				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				string inner = " 4 D_CTLM {TYPE:21,VALUE:0;" + safe2string(msg->authentication_code,sizeof(msg->authentication_code)) + ";" ;
				inner += safe2string( msg->access_point_name, sizeof(msg->access_point_name)) + ";" ;
				inner += safe2string( msg->username, sizeof(msg->username) ) + ";" ;
				inner += safe2string( msg->password, sizeof(msg->password) ) + ";" ;
				inner += safe2string( msg->server_ip , sizeof(msg->server_ip) ) + ";" ;
				inner += uitodecstr( msg->tcp_port ) + ";" ;
				inner += uitodecstr( msg->udp_port ) + ";0} \r\n" ;

				UpCtrlMsgEmergencyMonitoringAck resp;

				resp.header.msg_type    = ntouv16( UP_CTRL_MSG ) ;
				resp.header.msg_len 	= ntouv32(sizeof(UpCtrlMsgEmergencyMonitoringAck));
				resp.header.access_code = ntouv32( user._access_code ) ;
				resp.header.msg_seq 	= msg->header.msg_seq ;
				resp.ctrl_msg_header.data_type = ntouv16( UP_CTRL_MSG_EMERGENCY_MONITORING_ACK ) ;

				resp.ctrl_msg_header.vehicle_color = msg->ctrl_msg_header.vehicle_color;
				safe_memncpy(resp.ctrl_msg_header.vehicle_no, msg->ctrl_msg_header.vehicle_no, sizeof(resp.ctrl_msg_header.vehicle_no) );

				resp.ctrl_msg_header.data_length = ntouv32( sizeof(unsigned char) );
				resp.result 			= 0x00;
				//添加到缓存队列中
				_pEnv->GetMsgCache()->AddData( szKey, (const char *)&resp, sizeof(resp));

				int szlen = 0;
				char szbuf[1024] = {0} ;
				szlen += snprintf(szbuf + szlen, 1024 - szlen, "CAITS %s %s", szKey, macidDst.c_str());
				szlen += snprintf(szbuf + szlen, 1024 - szlen, "%s \r\n",  inner.c_str());

				((IMsgClient*)_pEnv->GetMsgClient())->HandleUpMsgData( macidDst.c_str(), szbuf, szlen);

				//_srvCaller.getTernimalByVehicleByTypeEx( DOWN_CTRL_MSG_EMERGENCY_MONITORING_REQ, seqid, carnum , carcolor, inner.c_str() ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_CTRL_MSG_EMERGENCY_MONITORING_REQ seqid %u, carnum %s, carcolor %s" , seqid , carnum, carcolor ) ;
		    }
			break ;
		}
	}
	else if (msg_type == DOWN_WARN_MSG ) {  // 从链路报警信息交互消息
		WarnMsgHeader *warnheader = ( WarnMsgHeader *) (data + sizeof(Header)) ;
		int data_type = ntouv16( warnheader->data_type ) ;

		char carnum[128]= {0};
		safe_memncpy( carnum, warnheader->vehicle_no, sizeof(warnheader->vehicle_no) ) ;
		char carcolor[128] = {0} ;
		sprintf( carcolor, "%d" , warnheader->vehicle_color ) ;

		char macidSrc[32];
		snprintf(macidSrc, 32, "%s_%s", carcolor, carnum);

		string macidDst = "";
		if( ! _pEnv->GetRedisCache()->HGet("KCTX.PLATE2SIM", macidSrc, macidDst) || macidDst.empty()) {
			OUT_WARNING(user._ip.c_str(), user._port, user._user_name.c_str(), "KCTX.PLATE2SIM: %s not exist", macidSrc);
			return;
		}

		switch( data_type ) {
		case DOWN_WARN_MSG_URGE_TODO_REQ:  // 报警督办请求
			{
				if ( len < (int) sizeof(DownWarnMsgUrgeTodoReq) ) {
					OUT_ERROR( ip, port, str_access_code.c_str(), "DOWN_WARN_MSG_URGE_TODO_REQ data length %d error" , len ) ;
					return ;
				}

				CBase64 base64;

				DownWarnMsgUrgeTodoReq *req = ( DownWarnMsgUrgeTodoReq *) data ;

				UpWarnMsgUrgeTodoAck resp ;
				resp.header.msg_len				   = ntouv32( sizeof(UpWarnMsgUrgeTodoAck) ) ;
				resp.header.access_code 		   = ntouv32( user._access_code ) ;
				resp.header.msg_seq     		   = req->header.msg_seq ;
				resp.warn_msg_header.vehicle_color = req->warn_msg_header.vehicle_color;
				safe_memncpy(resp.warn_msg_header.vehicle_no, req->warn_msg_header.vehicle_no, sizeof(resp.warn_msg_header.vehicle_no) );

				resp.warn_msg_header.data_length   = ntouv32( sizeof(int) + sizeof(char) ) ;
				resp.supervision_id     		   = req->warn_msg_body.supervision_id ;
				resp.result						   = 0x00 ;

				unsigned int seqid = _pEnv->GetSequeue() ;
				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				_pEnv->GetMsgCache()->AddData( szKey, (const char *)&resp, sizeof(resp) ) ;

				char supervisionEndUtc[128]={0};
				sprintf( supervisionEndUtc, "%llu", (unsigned long long)ntouv64(req->warn_msg_body.supervision_endtime) ) ;

				char supervisionId[128] = {0} ;
				sprintf( supervisionId , "%d" , ntouv32(req->warn_msg_body.supervision_id) ) ;

				char supervisionLevel[128] = {0} ;
				sprintf( supervisionLevel, "%d" , req->warn_msg_body.supervision_level ) ;

				char supervisor[256] = {0} ;
				safe_memncpy( supervisor, req->warn_msg_body.supervisor, sizeof(req->warn_msg_body.supervisor) ) ;

				char supervisorEmail[128] = {0} ;
				safe_memncpy( supervisorEmail, req->warn_msg_body.supervisor_email, sizeof(req->warn_msg_body.supervisor_email) ) ;

				char supervisorTel[128] = {0} ;
				safe_memncpy( supervisorTel, req->warn_msg_body.supervisor_tel, sizeof(req->warn_msg_body.supervisor_tel) ) ;

				char wanSrc[128] = {0} ;
				sprintf( wanSrc, "%d", req->warn_msg_body.warn_src ) ;

				char wanType[128] = {0} ;
				sprintf( wanType, "%d", ntouv16( req->warn_msg_body.warn_type ) ) ;

				char warUtc[128] = {0} ;
				sprintf( warUtc , "%llu", (unsigned long long)ntouv64(req->warn_msg_body.warn_time) ) ;

				char keyBuf[32];
				snprintf(keyBuf, 32, "%s_%s", carcolor, carnum);

				string inner = "CAITS " + string(szKey) + " " + macidDst + " 4 L_PROV {TYPE:D_WARN,WARNTODO";
				inner += ":" + string(wanSrc);
				inner += "|" + string(wanType);
				inner += "|" + string(warUtc);
				inner += "|" + string(supervisionId);
				inner += "|" + string(supervisionEndUtc);
				inner += "|" + string(supervisionLevel);
				inner += "|" + string(supervisor);
				inner += "|" + string(supervisorTel);
				inner += "|" + string(supervisorEmail);
				inner += "} \r\n";

				((IMsgClient*)_pEnv->GetMsgClient())->HandleUpMsgData( macidDst.c_str(), inner.c_str(), inner.length() );

				//_srvCaller.addMsgUrgeTodo( DOWN_WARN_MSG_URGE_TODO_REQ , seqid , supervisionEndUtc , supervisionId ,
				//		supervisionLevel , supervisor , supervisorEmail , supervisorTel , carcolor, carnum , wanSrc , wanType , warUtc ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_WARN_MSG_URGE_TODO_REQ seqid %u, carnum %s, carcolor %s" , seqid , carnum, carcolor ) ;
			}
			break ;
		case DOWN_WARN_MSG_INFORM_TIPS: // 报警预警
		case DOWN_WARN_MSG_EXG_INFORM:  // 实时交换报警信息
			{
				if ( len < (int) sizeof(DownWarnMsgInformTips) ) {
					OUT_ERROR( ip, port, str_access_code.c_str(), "%s data length %d error" ,
							( data_type == DOWN_WARN_MSG_EXG_INFORM ) ? "DOWN_WARN_MSG_EXG_INFORM" : "DOWN_WARN_MSG_INFORM_TIPS" , len ) ;
					return ;
				}

				DownWarnMsgInformTips *req = ( DownWarnMsgInformTips *) data ;

				int nlen = ntouv32(req->warn_msg_body.warn_length) ;
				if ( len < ( int ) sizeof(req) + nlen || nlen < 0 ) {
					OUT_ERROR( ip, port, str_access_code.c_str(), "%s data length %d error" ,
							( data_type == DOWN_WARN_MSG_EXG_INFORM ) ? "DOWN_WARN_MSG_EXG_INFORM" : "DOWN_WARN_MSG_INFORM_TIPS" , len ) ;
					return ;
				}

				unsigned int seqid = _pEnv->GetSequeue() ;
				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				CQString content ;
				content.SetString( (const char *)(data+sizeof(DownWarnMsgInformTips)) , nlen ) ;

				CBase64 base64;
				base64.Encode(content.GetBuffer(), content.GetLength());

				char alarmFrom[128] = {0} ;
				sprintf( alarmFrom, "%d", req->warn_msg_body.warn_src ) ;

				char alarmTime[128] = {0} ;
				sprintf( alarmTime, "%llu", (unsigned long long)ntouv64(req->warn_msg_body.warn_time) ) ;

				char alarmType[128] = {0} ;
				sprintf( alarmType, "%d", ntouv16(req->warn_msg_body.warn_type) ) ;

				string inner = "CAITS " + string(szKey) + " " + macidDst + " 4 L_PROV {TYPE:D_WARN,WARNTIPS";
				inner += ":" + string(alarmFrom);
				inner += "|" + string(alarmType);
				inner += "|" + string(alarmTime);
				inner += "|" + string(base64.GetBuffer());
				inner += "} \r\n";

				((IMsgClient*)_pEnv->GetMsgClient())->HandleUpMsgData( macidDst.c_str(), inner.c_str(), inner.length() );

				// 调用服务保存
				//_srvCaller.addMsgInformTips( data_type , seqid , content.GetBuffer() , alarmFrom, alarmTime, alarmType , carcolor , carnum ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "%s seqid %u, carnum %s, carcolor %s" , ( data_type == DOWN_WARN_MSG_EXG_INFORM ) ? "DOWN_WARN_MSG_EXG_INFORM" : "DOWN_WARN_MSG_INFORM_TIPS", seqid , carnum, carcolor ) ;
			}
			break ;
		}
	}
	else if ( msg_type == DOWN_EXG_MSG ) {  // 从链路动态信息交换消息
		ExgMsgHeader *msgheader = (ExgMsgHeader*) (data + sizeof(Header));
		int data_type = ntouv16( msgheader->data_type ) ;

		char carnum[128]= {0};
		safe_memncpy( carnum, msgheader->vehicle_no, sizeof(msgheader->vehicle_no) ) ;
		char carcolor[128] = {0} ;
		sprintf( carcolor, "%d" , msgheader->vehicle_color ) ;

		switch( data_type ) {
		case DOWN_EXG_MSG_REPORT_DRIVER_INFO: // 上报车辆驾驶员身份识别信息请求
			{
				if ( len < (int)sizeof(DownExgMsgReportDriverInfo) ) {
					OUT_ERROR( ip, port, str_access_code.c_str(), "DOWN_EXG_MSG_REPORT_DRIVER_INFO data length %d error" , len ) ;
					return ;
				}
				//http 查询服务
				DownExgMsgReportDriverInfo *msg = (DownExgMsgReportDriverInfo *)data;

				UpExgMsgReportDriverInfo resp;
				resp.header.msg_len 	= ntouv32(sizeof(UpExgMsgReportDriverInfo));
				resp.header.access_code = ntouv32( user._access_code ) ;
				resp.header.msg_seq 	= msg->header.msg_seq;
				resp.header.msg_type    = ntouv16( UP_EXG_MSG);
				resp.exg_msg_header.data_type     = ntouv16(UP_EXG_MSG_REPORT_DRIVER_INFO_ACK);
				resp.exg_msg_header.vehicle_color = msg->exg_msg_header.vehicle_color;
				safe_memncpy(resp.exg_msg_header.vehicle_no, msg->exg_msg_header.vehicle_no, sizeof(resp.exg_msg_header.vehicle_no) );
				resp.exg_msg_header.data_length = ntouv32(  sizeof(UpExgMsgReportDriverInfo) - sizeof(Header) - sizeof(ExgMsgHeader) - sizeof(Footer) );

				unsigned int seqid = _pEnv->GetSequeue() ;
				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				// 添加到缓存中
				_pEnv->GetMsgCache()->AddData( szKey, (const char *)&resp, sizeof(resp) ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_EXG_MSG_REPORT_DRIVER_INFO seqid %u, carnum %s, carcolor %s" , seqid , carnum, carcolor ) ;
			}
			break ;
		case DOWN_EXG_MSG_TAKE_WAYBILL_REQ: // 上报车辆电子运单请求
			{
				DownExgMsgTakeWaybillReq *msg = (DownExgMsgTakeWaybillReq *)data;

				UpExgMsgReportEwaybillInfo  resp;
				resp.header.msg_len 	= 0 ;
				resp.header.access_code = ntouv32( user._access_code ) ;
				resp.header.msg_seq 	= msg->header.msg_seq;
				resp.header.msg_type    = ntouv16( UP_EXG_MSG );
				resp.exg_msg_header.data_type     = ntouv16(UP_EXG_MSG_TAKE_WAYBILL_ACK);
				resp.exg_msg_header.vehicle_color = msg->exg_msg_header.vehicle_color;
				safe_memncpy(resp.exg_msg_header.vehicle_no, msg->exg_msg_header.vehicle_no, sizeof(resp.exg_msg_header.vehicle_no) );

				unsigned int seqid = _pEnv->GetSequeue() ;
				char szKey[256]={0};
				_pEnv->GetCacheKey( seqid, szKey ) ;

				// 添加到缓存中
				_pEnv->GetMsgCache()->AddData( szKey, (const char *)&resp, sizeof(resp) ) ;

				// 调用服务保存
				//_srvCaller.getEticketByVehicle( DOWN_EXG_MSG_TAKE_WAYBILL_REQ, seqid , carnum , carcolor ) ;

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_EXG_MSG_TAKE_WAYBILL_REQ seqid %u, carnum %s, carcolor %s" , seqid , carnum, carcolor ) ;
			}
			break ;
		}

	}
	else if ( msg_type == DOWN_BASE_MSG ) { // 从链路静态信息交换消息
		BaseMsgHeader *base_header = ( BaseMsgHeader *) ( data + sizeof(Header) ) ;
		int data_type = ntouv16( base_header->data_type ) ;

		char carnum[128]= {0};
		safe_memncpy( carnum, base_header->vehicle_no, sizeof(base_header->vehicle_no) ) ;
		char carcolor[128] = {0} ;
		sprintf( carcolor, "%d" , base_header->vehicle_color ) ;

		char macidSrc[32];
		snprintf(macidSrc, 32, "%s_%s", carcolor, carnum);

		string macidDst = "";
		if( ! _pEnv->GetRedisCache()->HGet("KCTX.PLATE2SIM", macidSrc, macidDst) || macidDst.empty()) {
			OUT_WARNING(user._ip.c_str(), user._port, user._user_name.c_str(), "KCTX.PLATE2SIM: %s not exist", macidSrc);
			return;
		}

		switch( data_type ) {
		case DOWN_BASE_MSG_VEHICLE_ADDED:
			{
				if ( len < (int)sizeof(DownBaseMsgVehicleAdded) ) {
					OUT_ERROR( ip, port, str_access_code.c_str(), "DOWN_BASE_MSG_VEHICLE_ADDED data length %d error" , len ) ;
					return ;
				}

				DownBaseMsgVehicleAdded *msg = (DownBaseMsgVehicleAdded*)data ;

				// 补报车辆静态信息
				UpbaseMsgVehicleAddedAck resp ;
				resp.header.msg_seq		= msg->header.msg_seq ;
				resp.header.access_code = ntouv32( user._access_code ) ;
				resp.msg_header.vehicle_color = base_header->vehicle_color ;
				safe_memncpy( resp.msg_header.vehicle_no, base_header->vehicle_no, sizeof(base_header->vehicle_no) ) ;

				string carinfo = "";
				if( ! _pEnv->GetRedisCache()->HGet("KCTX.CARINFO", macidSrc, carinfo) || carinfo.empty()) {
					OUT_WARNING(user._ip.c_str(), user._port, user._user_name.c_str(), "KCTX.CARINFO: %s not exist", macidSrc);
					return;
				}

				resp.header.msg_len  		= ntouv32(sizeof(UpbaseMsgVehicleAddedAck) + carinfo.length() + sizeof(Footer) ) ;
				resp.msg_header.data_length = ntouv32(carinfo.length()) ;

				DataBuffer buf;
				Footer footer;

				buf.writeBlock(&resp , sizeof(UpbaseMsgVehicleAddedAck));
				buf.writeBlock(carinfo.c_str(), carinfo.length());
				buf.writeBlock( &footer, sizeof(footer) ) ;

				HandlePasUpData(access_code, buf.getBuffer(), buf.getLength());

				OUT_PRINT( ip, port , str_access_code.c_str(), "DOWN_BASE_MSG_VEHICLE_ADDED , carnum %s, carcolor %s" , carnum, carcolor ) ;
			}
			break ;
		}
	} else {
		OUT_WARNING( ip , port , str_access_code.c_str(), "except message:%s", (const char*)data );
	}

	user._last_active_time = time(NULL) ;
	_online_user.SetUser( user._user_id, user ) ;
}

// 关闭主链路的连接请求
void PasClient::Close( int accesscode )
{
	User user = _online_user.GetUserByAccessCode( accesscode ) ;
	if ( user._user_id.empty() ) {
		OUT_ERROR( NULL, 0, NULL, "close access code %d user not exist" , accesscode ) ;
		return ;
	}
	user._user_state = User::OFF_LINE ;
	_online_user.SetUser( user._user_id, user ) ;
}

// 更新当前从链路的连接状态
void PasClient::UpdateSlaveConn( int accesscode, int state )
{
	User user = _online_user.GetUserByAccessCode( accesscode ) ;
	if ( user._user_id.empty() ) {
		OUT_ERROR( NULL, 0, NULL, "update slave close access code %d user not exist", accesscode ) ;
		return ;
	}
	// 处理连接状态更新
	//_srvCaller.updateConnectState( ( state == CONN_CONNECT ) ? DOWN_CONNECT_RSP : DOWN_DISCONNECT_RSP ,
	//		_pEnv->GetSequeue() , GetAreaCode(user), CONN_SLAVER ,  state ) ;
}

// 直接断开对应省的连接处理
void PasClient::Enable( int areacode , int flag )
{
	char szuser[128] = {0};
	sprintf( szuser, "%s%d" , PAS_USER_TAG , areacode ) ;

	User user = _online_user.GetUserByUserId( szuser ) ;
	if ( user._user_id.empty() ) {
		OUT_ERROR( NULL, 0, NULL, "Enable areacode user %d failed" , areacode ) ;
		return ;
	}

	// 是否开启重连
	if ( flag & PAS_USERLINK_ONLINE ) {
		OUT_INFO(user._ip.c_str(),user._port,user._user_id.c_str(),"Send user state offline then reconnect");
		// 重新设置为离线重连
		if ( user._user_state != User::ON_LINE ) {
			user._user_state = User::OFF_LINE ;
		}
	} else if ( flag & PAS_MAINLINK_LOGOUT ){
		OUT_INFO(user._ip.c_str(),user._port,user._user_id.c_str(),"Send UpDisconnectReq, UP_DISCONNECT_REQ");
		// 断开连接
		UpDisconnectReq req ;
		req.header.msg_seq 		= ntouv32(_proto_parse.get_next_seq());
		req.header.access_code  = ntouv32( user._access_code);
		req.user_id        		= ntouv32( atoi(user._user_name.c_str()) ) ;
		memcpy( req.password , user._user_pwd.c_str(), sizeof(req.password) ) ;

		SendCrcData( user._fd , (const char*)&req, sizeof(req) );

		user._user_state = User::DISABLED ;

	} else if ( flag & PAS_SUBLINK_ERROR ) {  // 处理从链路异常的情况
		OUT_INFO( user._ip.c_str(), user._port, user._user_id.c_str(), "PAS_SUBLINK_ERROR : UP_CLOSELINK_INFORM close double link" ) ;
		// 从链路异常处理
		_pEnv->GetPccServer()->Close( user._access_code, UP_CLOSELINK_INFORM, 0x00 ) ;
		// 模拟认为异常直接关闭
		if ( user._fd > 0 ) {
			CloseSocket( user._fd ) ;
			user._user_state = User::OFF_LINE ;
		}
	} else if ( flag & PAS_MAINLINK_ERROR ) { // 处理主链路异常的情况
		OUT_INFO( user._ip.c_str(), user._port, user._user_id.c_str(), "PAS_MAINLINK_ERROR : UP_DISCONNECT_INFORM message" ) ;
		// 发送关闭从链路请求
		_pEnv->GetPccServer()->Close( user._access_code , UP_DISCONNECT_INFORM , 0x00 ) ;
		/**
		// 模拟认为异常直接关闭
		if ( user._fd > 0 ) {
			CloseSocket( user._fd ) ;
			user._user_state = User::OFF_LINE ;
		}*/
	} else {  // 错误命令
		OUT_ERROR( user._ip.c_str(), user._port, user._user_id.c_str(), "Recv Error Enable flag %x", flag ) ;
	}
	// 设置状态为禁用
	_online_user.SetUser( szuser , user ) ;
}

// 添加MACID到SEQID的映射关系
void PasClient::AddMacId2SeqId( unsigned short msgid, const char *macid, const char *seqid )
{
	// 处理响应和请求生成序列,0x8000
	if ( ! ( msgid & 0x8000 ) ) {
		msgid |= 0x8000 ;
	}

	char key[512] = {0};
	// MAC和消息类型两种对应产生的序号
	sprintf( key, "%s_%d" , macid, msgid ) ;

	_macid2seqid.AddSession( key, seqid ) ;
}

// 通过MACID和消息内容取得对应数据
bool PasClient::GetMacId2SeqId( unsigned short msgid, const char *macid, char *seqid )
{
	// 处理响应和请求生成序列,0x8000
	if ( ! ( msgid & 0x8000 ) ) {
		msgid |= 0x8000 ;
	}

	char key[512] = {0};
	// MAC和消息类型两种对应产生的序号
	sprintf( key, "%s_%d" , macid, msgid ) ;

	string val ;
	if ( ! _macid2seqid.GetSession( key, val ) ){
		return false ;
	}
	sprintf( seqid, "%s" , val.c_str() ) ;

	return true ;
}

// 取得当前用户的区域编码
int PasClient::GetAreaCode( User &user )
{
	if ( user._user_id.empty() ) {
		return 0 ;
	}
	return atoi((const char *)( user._user_id.c_str() + PAS_TAG_LEN ));
}

// 加密处理数据
bool PasClient::EncryptData( unsigned char *data, unsigned int len , bool encode )
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
	// printf( "M1: %d, IA1: %d, IC1: %d\n" , M1, IA1, IC1 ) ;

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

//========================================= 用户处理  ===============================================
// 从USERINFO转换为User处理
void PasClient::ConvertUser( const _UserInfo &info, User &user )
{
	user._user_id     =  info.tag + info.code ;
	user._access_code =  atoi( info.type.c_str() ) ;  // 处理接入码
	user._ip          =  info.ip ;
	user._port        =  info.port ;
	user._user_name   =  info.user ;
	user._user_pwd    =  info.pwd  ;
	user._user_type   =  info.type ;
	user._user_state  = User::OFF_LINE ;
	user._socket_type = User::TcpConnClient ;
	user._connect_info.keep_alive = AlwaysReConn ;
	user._connect_info.timeval    = 30 ;
}

void PasClient::NotifyUser( const _UserInfo &info , int op )
{
	string key = info.tag + info.code ;
	User user  = _online_user.GetUserByUserId( key ) ;

	OUT_PRINT( info.ip.c_str(), info.port, key.c_str() , "PasClient operate %d user, username %s, password %s" ,
				op , info.user.c_str(), info.pwd.c_str() ) ;

	switch( op ){
	case USER_ADDED:
		{
			ConvertUser( info, user ) ;
			// 添加新的用户
			if ( ! _online_user.AddUser( key, user ) ) {
				if ( user._fd != NULL ) {
					OUT_INFO( user._ip.c_str(), user._port, user._user_id.c_str() ,
							"PasClient Add New user close fd %d" , user._fd->_fd ) ;
					CloseSocket( user._fd ) ;
				}
				_online_user.SetUser( key, user ) ;
			}
		}
		break ;
	case USER_DELED:
		if ( ! user._user_id.empty() ) {
			if ( user._fd != NULL ) {
				OUT_INFO( user._ip.c_str(), user._port, user._user_id.c_str() ,
						"PasClient Delete User fd %d" , user._fd->_fd ) ;
				CloseSocket( user._fd ) ;
			}
			// 删除用户处理
			_online_user.DeleteUser( key ) ;
		}
		break ;
	case USER_CHGED:
		if ( ! user._user_id.empty() ) {
			// 修改用户数据
			ConvertUser( info, user ) ;
			if ( user._fd != NULL ) {
				OUT_INFO( user._ip.c_str(), user._port, user._user_id.c_str() ,
						"PasClient Change User close fd %d" , user._fd->_fd ) ;
				CloseSocket( user._fd ) ;
			}
			_online_user.SetUser( key, user ) ;
		}
		break ;
	}
}

