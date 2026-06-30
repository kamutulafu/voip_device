#ifndef _WXVOIP_CLIENT_H
#define _WXVOIP_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "wxvoip_network_impl.h"
#include "wxvoip_os_impl.h"

typedef enum wx_cloudvoip_session_type {
  WX_CLOUDVOIP_SESSION_VIDEO = 0,  // 音视频通话
  WX_CLOUDVOIP_SESSION_AUDIO = 1,  // 纯音频通话
} wx_cloudvoip_session_type_t;

typedef enum wx_cloudvoip_member_camera_status {
  WX_CLOUDVOIP_MEMBER_CAMERA_STATUS_OPEN = 0,   // 该成员摄像头开启
  WX_CLOUDVOIP_MEMBER_CAMERA_STATUS_CLOSE = 1,  // 该成员摄像头关闭
} wx_cloudvoip_member_camera_status_t;

typedef enum wx_wxa_flavor {
  WX_WXA_FLAVOR_RELEASE = 0,  // 小程序正式版
  WX_WXA_FLAVOR_DEBUG = 1,    // 小程序开发版
  WX_WXA_FLAVOR_DEMO = 2,     // 小程序体验版
} wx_wxa_flavor_t;

typedef enum wx_sdk_debug_level {
  WX_VOIPSDK_DEBUG_ERROR = 0,  // SDK 只利用 os_printf 输出错误日志
  WX_VOIPSDK_DEBUG_INFO = 1,  // SDK 利用 os_printf 输出一些中间日志
} wx_sdk_debug_level_t;

typedef struct wx_cloudvoip_config {
    char *data_dir;
    char *device_id;
    char *model_id;
    char *wxa_appid;
    wx_wxa_flavor_t wxa_flavor;
    wx_sdk_debug_level_t log_level;
} wx_cloudvoip_config_t;

/**
 * @brief 表示一个 VoIP 角色, 可以是拨打方, 也可以是被拨打方.
 *
 */
typedef struct wx_cloudvoip_member {
  /**
   * @brief
   * 对于设备：
   * IoT VoIP 模式下该项填 "" (该项由厂商小程序调用
   * `wx.requestDeviceVoIP` 时设置, 这里必须填空字符串,
   * 由微信后端替换成实际设备名称).
   *
   * 校园模式下该项填学生名称.
   *
   * 对于被拨打的微信用户：
   * 该项填微信用户昵称（显示在通话页面中，可以填房间号之类的名称）
   */
  const char* name;

  /**
   * @brief
   * 对于设备：
   * IoT VoIP 模式下该项填 设备 ID (SN).
   * 校园模式下该项填学生 ID.
   *
   * 对于被拨打的微信用户:
   * 微信用户的 OpenID.
   * OpenID 需要在微信手机客户端小程序侧通过 `wx.login` 获取,
   * 并通过你自建的某种通信渠道传递到设备上后，填到此项.
   */
  const char* id;

  /**
   * @brief 表示该 VoIP 通话成员是否开启摄像头
   * 对于设备：
   * 如果设备不具备摄像头，该项填 WX_VOIP_MEMBER_CAMERA_STATUS_CLOSE.
   * 如果设备具备摄像头，该项填 WX_VOIP_MEMBER_CAMERA_STATUS_OPEN.
   *
   * 对于被拨打的微信用户：
   * 如果设备不具备屏幕，或者不需要查看被拨打的微信用户的摄像头内容，该项填
   * WX_VOIP_MEMBER_CAMERA_STATUS_CLOSE.
   * 如果设备具备屏幕，且需要查看被拨打的微信用户的摄像头内容，该项填
   * WX_VOIP_MEMBER_CAMERA_STATUS_OPEN.
   */
  wx_cloudvoip_member_camera_status_t camera_status;
} wx_cloudvoip_member_t;

/**
 * @brief 初始化 voip
 *
 * 调用其它接口之前需要调用 wx_init() 初始化
 *
 *
 * @param stack (nonnull) 开发者实现 TLS 连接、关闭、读、写接口。
 * @param hal (nonnull) 开发者实现 OS 相关的接口
 * @param config (nonnull) 设备配置信息
 * @return
 *  - WXERROR_RESOURCE_EXHAUSTED: 内存资源分配失败
 *  - WXERROR_INVALID_DEVICEID: 设备已注册，但当前 sdk 用的 deviceid、modelid 不对
 */
wx_error_t wx_init(wxvoip_network_https_impl_t *stack, wxvoip_os_impl_t *hal, wx_cloudvoip_config_t *config);

/**
 * @brief 销毁 voip
 * 
 * 进行资源释放
 */
void wx_destory(void);

/**
 * @brief 注册设备
 * 
 * 需要调用一次此接口，若设备已经注册，则立即返回，若设备未注册或注册数据错误，会再注册。
 *
 * @param sn_ticket (nonnull) 小程序对应的 snticket，参考：
 * https://developers.weixin.qq.com/miniprogram/dev/OpenApiDoc/hardware-device/getSnTicket.html
 * @return
 *  - WXERROR_RESOURCE_EXHAUSTED: 内存资源分配失败
 *  - WXERROR_RESPONSE: 后台返回失败，一般是网络问题
 *  - WXERROR_IO: IO 失败，一般是写文件接口返回失败
 *  - 其它：
 *       -10008: snticket 有问题
 */
wx_error_t wx_device_register(const char *sn_ticket);

/**
 * @brief 检测设备是否已经被注册
 * 需要在 wx_init 调用完成之后才能调用本函数.
 *
 * @param is_registered_out 输出设备注册情况.
 * @return 检测过程中是否出现错误, 比如数据文件夹不可读写, 或者 rpmb
 * 设备不能被正常访问.
 *   - WXERROR_OK: 正确返回, 可以正确使用 is_registered_out 的值来判断设备是否注册过
 *   - WXERROR_FAILED_PRECONDITION: wx_init 未被调用
 *   - WXERROR_INVALID_ARGUMENT: 参数为空
 *   - WXERROR_INVALID_DEVICEID: 设备已注册，但当前 sdk 用的 deviceid、modelid 不对
 *   - WXERROR_UNKNOWN: 注册信息被破坏，这种情况下，一般可以清理设备重新注册
 */
wx_error_t wx_device_is_registered(int* is_registered_out);

/**
 * @brief 拨打 VoIP 通话给指定的微信用户
 *
 * 厂商需要自行实现用户的通讯录功能.
 * 厂商需要在自己的小程序内提示用户向指定的设备 (设备 ID) 授权 VoIP 通话权限后,
 * 方可在携带该设备 ID 的设备上向该用户拨打 VoIP 通话.
 *
 * @param room_type 通话类型，有音频和音视频两种
 * @param caller 本设备的 VoIP 角色设置.
 * @param callee 要拨打的微信用户的 VoIP 角色设置
 * @param custom_query 小程序页面自定义参数, 可以传 NULL
 * @param payload 第三方云 SDK 标记 VoIP 流的标识符（比如 json）
 * @return
 *   - WXERROR_FAILED_PRECONDITION: 不能发起通话请求，比如获取 token 不对等.
 *   - WXERROR_RESOURCE_EXHAUSTED: 内存资源分配失败
 *   - WXERROR_RESPONSE: 后台返回失败，一般是网络问题
 *   - 负数：请求通话时后台返回了失败码取负，可参考https://mp.weixin.qq.com/wxopen/plugindevdoc?appid=wxf830863afde621eb
 *       1	roomid 错误
 *       2	设备 deviceId 错误
 *       3	voip_id 错误
 *       4	校园场景支付刷脸模式，voipToken 错误
 *       5	生成 voip 房间错误
 *       7	openId 错误
 *       8	openId 未授权
 *       9	校园场景支付刷脸模式：openId 不是 userId 的联系人；硬件设备模式：openId 未绑定设备
 *       12	小程序音视频能力审核未完成，正式版中暂时无法使用
 *       13	硬件设备拨打手机微信模式，voipToken 错误
 *       14	手机微信拨打硬件设备模式，voipToken 错误
 *       15	音视频费用包欠费
 *       17	voipToken 对应 modelId 错误
 *       19	openId 与小程序 appId 不匹配。请注意同一个用户在不同小程序的 openId 是不同的
 *       20	openId 无效
 *       10008 snticket 过期
 */
wx_error_t
wx_cloudvoip_client_call(wx_cloudvoip_session_type_t room_type,
                         const wx_cloudvoip_member_t* caller,
                         const wx_cloudvoip_member_t* callee,
                         const char* custom_query,
                         const char* payload);

#ifdef __cplusplus
}
#endif
#endif
