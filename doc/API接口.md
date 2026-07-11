# 1、设备端接口

## POST 人脸搜索（唤醒设备） 

POST /wechat-service/api/device/searchFace2

上传人脸照片，返回联系人列表
此时执行唤醒积分逻辑（首次设备5积分，平常唤醒1积分）

返回值：
{
    "code": "0",
    "msg": "操作成功",
    "data": "JV9DDmqgoTmGEET+Neq0zt7mzuAkov53eWckWujN81RuCb/AGjT21/RLzjZQ0e7GQL94R5OzqTFN5D1pVqszSjZ2SfxZdtajVSVD54pHgkoVLRL6k20/FKiwV/hyaGv+L1HH3eYr5rGBVNhtoGZGwaB6dTnvq4quElj7Nue41Zo4s9AwqPjniB2BChACDpiPEXuOiDRzT7KAPnVr5/gJwkskB/YvkWr7niKluzcdGXEvcfqGLVsCb+15naiTxozydH2udTQAhrJxHelcor8F7D/9t0md9+IlSATvmvkowLM9SXxjibBzVmNUCvHxtJqr62LitmZQYrPX46qUOU1ddA=="
}
data的解密结果：
{"babyName":"邓轩","contacts":[{"mobile":"18611611591","userId":"1891311363519676416"}],"sessionid":"46d7bf7d-e5ad-4dcd-9efc-a7b10575302f"}

code=1，表示出错了：
如果图片没有检测到人脸，code=1，返回 msg：face is fuzzy
如果陌生人注册失败，code=1，返回msg：reg stranger fail

> Body 请求参数

```yaml
file://D:\oo\d1.jpg

```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|deviceid|header|string| 是 ||none|
|sid|header|string| 否 ||调用此接口返回的sessionId，第一次调用时传空字符不传|
|body|body|object| 否 ||none|
|» file|body|string(binary)| 否 ||none|

> 返回示例

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": {
        "babyId": "1891319385092521985",
        "babyName": "轩轩",
        "babyNick": null,
        "gender": "1",
        "age": null,
        "sessionid": "98aa31ea-4d62-4890-8e4d-423f2faf0510",
        "isRegStranger": 0,
        "isRegUser": 0,
        "contacts": [
            {
                "mobile": "18611611591",
                "relation": "baba",
                "userId": "1891311363519676416",
                "userName": "邓先生",
                "wxOpenId": "o1s5V7NBU6E15YczluK6EKPxk0mg",
                "answeringType": 0
            }
        ],
        "answeringType": null,
        "effecLeaveMsg": 0,
        "effecTelCall": 0,
        "effecWxVideo": 0
    }
}
```

```json
{
    "code": "0",
    "msg": "no user"
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|object|true|none||none|
|»» babyId|string|true|none||宝宝ID（也可能是UserId或StrangerID）|
|»» babyName|string|true|none||宝宝姓名|
|»» babyNick|null|true|none||宝宝昵称|
|»» gender|null|true|none||宝宝性别|
|»» age|null|true|none||年龄|
|»» sessionid|string|true|none||会话ID|
|»» isRegStranger|integer|true|none||是否在册陌生人（1=是，0=否）|
|»» isRegUser|integer|true|none||是否在册用户（1=是，0=否）|
|»» contacts|null|true|none||联系人列表|
|»» answeringType|integer|false|none||优先接听电话类型（0=电话，1=微信）|
|»» effecLeaveMsg|integer|false|none||剩余留言次数|
|»» effecTelCall|integer|false|none||剩余电话时长（分钟）|
|»» effecWxVideo|integer|false|none||剩余微信语音时长（分钟）|
|»» equipmentStatus|integer|false|none||会员状态：1=公益版，2=商业版，null=其他|

## POST 获取设备配置

POST /wechat-service/api/device/getDeviceConfig

配置版本号：如果返回的版本号大于本地的，需要更新本地配置版本号和配置；如果版本号相同，仅返回公共配置信息
接口返回示例：
{
    "code": "0",
    "msg": "操作成功",
    "data": "xftURM663OkWkKt+w5VAw+LRw/dd4+30n8Ebvmx8PRz2Ju1jSlBnTtOr8Rtd8bB3c2wPZin1fBnk58nMp7GXyJVPkGdg7d9eO6xhx9YzHJz5zE/JuJfDUTqhpdqyg7ExOOnirILNSfG4trHJX0aPCY63R+9dKWXQ2YuCWUJhqwhMOpBoGWZx3WHDcnvy0lp0KFh+V0gU8zBlBCQUsYgMTgd5u5qheZTpA/PGrOwuwBEYSS8sWY8KK7twXnQHM9OjQcmfUveWCN6SDJ/ITTSmNy3FnpL5IKgexBU1mmHWTSI/SekTdQ6Jp8pSddC6cLjnVgt3EdiAARFB31EV3bSjV17HpwY3uOqs+I871jN3w3+MuAMk8lXh04ymc7j8TPtIj+lMM7tZqkWgvoZ5OZbV1HUpNG9Thb9hcksM4fHIkFwb+siYcvWRDrwAEYJTGp12SL2v1Firc6hjXMgvF3tVy8mD5nErC4CkstbjtLIvzGfLW7qWkipcMbTGlMvGO/aJMbdky2WOJokVKodJWEPv6mt3rHY6/Xnmjk1sG9oTZyWL8oUQiCFFlOgyZAqjgJ2AffEu7JAiwX8m9xMHlPOvfUkW49moeejURvRZ0i++9U9k6XhFObxUo7dJiAl8ZPZPaiI4zT0USgiUuRILZmO6Lyr40txw92q7K4rgfhHmyt0="
}
对data解密后如下：
{"cfgTime":600,"domain":"https://gateway.tdskynet.com","heartBeatTime":60,"pubKey":"MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDdJoUKqZrZv05mbAsvtonOQDQAH3Ev0nrr1EpEqI4Azs8H/bOLpJL3SjcqS92julNpiufU7KDpDJYkarj/lBLHgX0qlC5zn/Uwnb5NF165GyVMHpO6byRGV6ggnxmsaILd5XfA9EXwQqIU8kkPoab61hEEjklyDOx8kinfxIKxMQIDAQAB","sipAddr":"61.147.13.211:5060","sipName":"at174226584589319","sipPwd":"3C2DAE633D3FC521AC6C1E269EE18728","waitTime":60,"wxDeviceId":"wx_device_id_1"}

注意：
配置信息有版本号，版本号值越高越新。
本地版本号越小，说明需要更新，一般本地版本号和线上版本号值一样时，不需要更新本地配置。

**当服务器端版本号大于本地且内容有变化时，应采用服务端配置，本地更新为服务端配置。**

配置文件固定存储目录：Internal Memory/tdtec/zj/tdcfg.dat，配置文件内容格式如下（存储时base64-encode，读取时base64-decode）：
{
	"cfgTime": 600,
	"domain": "https://gateway.tdskynet.com",
	"heartBeatTime": 60,
	"pubKey": "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDdJoUKqZrZv05mbAsvtonOQDQAH3Ev0nrr1EpEqI4Azs8H/bOLpJL3SjcqS92julNpiufU7KDpDJYkarj/lBLHgX0qlC5zn/Uwnb5NF165GyVMHpO6byRGV6ggnxmsaILd5XfA9EXwQqIU8kkPoab61hEEjklyDOx8kinfxIKxMQIDAQAB",
	"sipAddr": "61.147.13.211:5060",
	"sipName": "at174226584589319",
	"sipPwd": "3C2DAE633D3FC521AC6C1E269EE18728",
	"waitTime": 60,
	"wxDeviceId": "wx_device_id_1",
	"deviceId": "wx_device_id_1"
}
说明：
deviceId：设备ID，由平台统一分配，设备初始化时，会将如上信息写入配置文件tdcfg.dat

> Body 请求参数

```json
{
    "id": "ZA2500000003",
    "vn": 1
}
```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|body|body|object| 否 ||none|
|» id|body|string| 是 ||设备ID|
|» vn|body|integer| 是 ||配置版本编号(自然数，数字越大表示越新)|

#### 详细说明

**» vn**: 配置版本编号(自然数，数字越大表示越新)
本地版本号越小，说明需要更新，一般本地版本号和线上版本号值一样时，不需要更新本地配置。
当服务器端版本号大于本地且内容有变化时，应采用服务端配置，本地更新为服务端配置。

> 返回示例

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": "xftURM663OkWkKt+w5VAw+LRw/dd4+30n8Ebvmx8PRz2Ju1jSlBnTtOr8Rtd8bB3c2wPZin1fBnk58nMp7GXyJVPkGdg7d9eO6xhx9YzHJz5zE/JuJfDUTqhpdqyg7ExOOnirILNSfG4trHJX0aPCY63R+9dKWXQ2YuCWUJhqwhMOpBoGWZx3WHDcnvy0lp0KFh+V0gU8zBlBCQUsYgMTgd5u5qheZTpA/PGrOwuwBEYSS8sWY8KK7twXnQHM9OjQcmfUveWCN6SDJ/ITTSmNy3FnpL5IKgexBU1mmHWTSI/SekTdQ6Jp8pSddC6cLjnVgt3EdiAARFB31EV3bSjV17HpwY3uOqs+I871jN3w3+MuAMk8lXh04ymc7j8TPtIj+lMM7tZqkWgvoZ5OZbV1HUpNG9Thb9hcksM4fHIkFwb+siYcvWRDrwAEYJTGp12SL2v1Firc6hjXMgvF3tVy8mD5nErC4CkstbjtLIvzGfLW7qWkipcMbTGlMvGO/aJMbdky2WOJokVKodJWEPv6mt3rHY6/Xnmjk1sG9oTZyWL8oUQiCFFlOgyZAqjgJ2AffEu7JAiwX8m9xMHlPOvfUkW49moeejURvRZ0i++9U9k6XhFObxUo7dJiAl8ZPZPaiI4zT0USgiUuRILZmO6Lyr40txw92q7K4rgfhHmyt0="
}
```

```json
{
    "cfgTime": 600,
    "heartBeatTime": 60,
    "waitTime": 60,
    "domain": "https://gateway.tdskynet.com"
}
```

```json
{
    "cfgTime": 600,
    "domain": "https://gateway.tdskynet.com",
    "heartBeatTime": 60,
    "lastVn": "1.0.4",
    "pubKey": "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDdJoUKqZrZv05mbAsvtonOQDQAH3Ev0nrr1EpEqI4Azs8H/bOLpJL3SjcqS92julNpiufU7KDpDJYkarj/lBLHgX0qlC52n/Uwnb5NF165GyVMHpO6byRGV6ggnxmsaILd5XfA9EXwQqIU8kkPoab61hEEjklyDOx8kinfxIKxMQIDAQAB",
    "sipAddr": "call.meiqia.com:5060",
    "sipName": "100010",
    "sipPwd": "S9NwgzWVUfaOH7T3HMRm1",
    "upgradeUrl": "https://www.pgyer.com/xeJGJwCS",
    "waitTime": 60,
    "wxDeviceId": "wx_device_id_1"
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» cfgTime|integer|true|none||配置信息更新同步请求间隔时长(秒)|
|» heartBeatTime|integer|true|none||心跳间隔时长(秒)|
|» pubKey|string|true|none||加密公钥|
|» sipAddr|string|true|none||SIP-IP地址（可能含端口）|
|» sipName|string|true|none||SIP-用户名|
|» sipPwd|string|true|none||SIP-密码|
|» waitTime|integer|true|none||空白无交互挂机等待时长(秒)|
|» wxDeviceId|string|true|none||微信设备ID|
|» domain|string|true|none||服务端接口域名(含协议部分)|
|» snTicket|string|true|none||设备在微信硬件平台的snTicket|
|» modelId|string|true|none||modelId|
|» wifiSsid|string|false|none||Wifi-SSID（wifi连接的设备）|
|» wifiPwd|string|false|none||Wifi-密码（wifi连接的设备）|
|» useType|integer|true|none||使用类型：0=收费使用（校外），1=免费使用（校内）|
|» volume|string|false|none||音量大小，示例：4/15，4表示当前音量值，15表示最大音量值。注意：这个音量一般设置多媒体、铃声、闹钟等（通话音量单独字段控制）。|
|» tVolume|string|false|none||通话音量大小，示例：4/15，4表示当前音量值，15表示最大音量值。|
|» ctrlPasswd|string|false|none||控制时的验证密码（切后台时的验证使用）|

## GET 心跳接口

GET /wechat-service/api/device/hrp

参数格式：RSA-Encode({设备ID}:{电量}:{4G信号强度}:{预留1}:{预留2}:{预留3}:{时间戳|毫秒13位})
电量：电量的百分比整数值
4G信号强度：
示例数据：test_device_id_1:85:1742354645169
实际传入：test_device_id_1:UJr1/irgHXF0HzZ1moHh0w9a/gsp3478xhZWaJhzsDmd6VZSkh88epCj25t3TJdiqlDf1Q7A431csb4x713eIBu9KfHXD632fs1KwjpKMubNHNSK1xQbSPWWgsyDUt66Vf98zWUTZHFXdkI6oW8WsVRfJ5bivWaweWZhs0PzDfo=
此接口暂定60秒1次

> Body 请求参数

```
test_device_id_1:UJr1/irgHXF0HzZ1moHh0w9a/gsp3478xhZWaJhzsDmd6VZSkh88epCj25t3TJdiqlDf1Q7A431csb4x713eIBu9KfHXD632fs1KwjpKMubNHNSK1xQbSPWWgsyDUt66Vf98zWUTZHFXdkI6oW8WsVRfJ5bivWaweWZhs0PzDfo=

```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|body|body|string| 否 ||none|

> 返回示例

> 200 Response

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": "ok"
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|string|true|none||none|

## GET APP启动时同步信息

GET /wechat-service/api/device/startupSync

参数格式：RSA-Encode({设备ID}:{电量}:{4G信号强度}:{版本号}:{wifi-ssid}:{入网方式}:{物联网卡号}:{操作系统类型}:{操作系统版本}:{时间戳|毫秒13位})
电量：电量的百分比整数值
4G信号强度：
示例数据：test_device_id_1:85:80:1.2.3::::::1742354645169
实际传入：test_device_id_1:UJr1/irgHXF0HzZ1moHh0w9a/gsp3478xhZWaJhzsDmd6VZSkh88epCj25t3TJdiqlDf1Q7A431csb4x713eIBu9KfHXD632fs1KwjpKMubNHNSK1xQbSPWWgsyDUt66Vf98zWUTZHFXdkI6oW8WsVRfJ5bivWaweWZhs0PzDfo=
此接口暂定60秒1次
入网方式：即上网类型，wifi/4g/line
物联网卡号：如果是4G上网，必须参数
操作系统类型：android/rtos

> Body 请求参数

```
test_device_id_1:UJr1/irgHXF0HzZ1moHh0w9a/gsp3478xhZWaJhzsDmd6VZSkh88epCj25t3TJdiqlDf1Q7A431csb4x713eIBu9KfHXD632fs1KwjpKMubNHNSK1xQbSPWWgsyDUt66Vf98zWUTZHFXdkI6oW8WsVRfJ5bivWaweWZhs0PzDfo=

```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|body|body|string| 否 ||none|

> 返回示例

> 200 Response

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": "ok"
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|string|true|none||none|

## POST 保存通话记录

POST /wechat-service/api/device/saveCallLog

> Body 请求参数

```json
{
    "babyId": "1891319385092521985",
    "userId": "1891311363519676416",
    "relation": "father",
    "type": 20,
    "status": 10,
    "callTime": "2025-03-18T04:59:01.0Z",
    "callDuration": 36,
    "deviceId": "test_device_id_1",
    "imgPath": "http://www.tdskynet.com/images/product1.png",
    "sessionId": "46d7bf7d-e5ad-4dcd-9efc-a7b10575302f",
    "address": "北京市朝阳区绿地金融中心1号机",
    "babyName": "轩轩"
}
```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|sessionid|header|string| 否 ||会话ID|
|body|body|object| 否 | t_baby_call_log|none|
|» babyId|body|string| 是 | 宝宝id|none|
|» babyName|body|string| 否 | 宝宝名称|none|
|» userId|body|string| 是 | 家长id（人脸识别时返回）|none|
|» relation|body|string| 是 | 关系（英文关系名称:father/mother/grandpa/grandma/brother/sister/friend）|none|
|» type|body|integer| 是 | 通话类型  10-视频通话  20-语音通话|none|
|» status|body|integer| 是 | 通话状态  10-已接听 20-未接听 |none|
|» callTime|body|string(date-time)| 是 | 通话时间|none|
|» callDuration|body|integer| 是 | 通话时长（单位：秒）|none|
|» deviceId|body|string| 是 | 通话设备id|none|
|» address|body|string| 否 | 通话地址|none|
|» imgPath|body|string| 是 | 通话时的设备截图|none|
|» sessionId|body|string| 是 | 会话ID|none|
|» relationDesc|body|string| 是 | 关系描述(用在小朋友给朋友的家长的情况，记录完整的关系名称)。当给好友的联系人打电话的情况，必传；其他情况不传|none|
|» ansBabyId|body|string| 是 | 接听方宝贝ID|none|
|» callStatus|body|string| 否 | 小程序返回的请求状态|none|
|» msg|body|string| 否 | 小程序返回的请求信息描述（errMsg）|none|
|» mobileGeo|body|string| 是 | 手机端坐标|从手机端返回|
|» mobileAddr|body|string| 是 | 手机端地址|从手机端返回|

> 返回示例

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": "ok"
}
```

```json
sessionId error
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|string|true|none||none|

## POST 获取留言

POST /wechat-service/api/device/getLeaveMsgPage

文件的存储见【公共方法】-【上传文件】接口

> Body 请求参数

```json
{
  "sessionId": "string"
}
```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|sessionid|header|string| 否 ||会话ID|
|body|body|object| 否 ||none|
|» sessionId|body|string| 是 ||会话ID|

> 返回示例

> 200 Response

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": {
        "count": 1,
        "page": 1,
        "size": 50,
        "list": [
            {
                "id": "d0c3ac4deacd483cbe8eb8a2a3ea88e1",
                "fromName": "家长",
                "relation": "father",
                "soundPath": "http://www.tdskynet.com/images/mm5.mp3"
            }
        ]
    }
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|object|true|none||none|
|»» count|integer|true|none||none|
|»» page|integer|true|none||none|
|»» size|integer|true|none||none|
|»» list|[object]|true|none||none|
|»»» id|string|false|none||none|
|»»» fromId|string|true|none||留言者ID|
|»»» fromName|string|false|none||留言者姓名|
|»»» relation|string|false|none||留言者与宝宝关系|
|»»» soundPath|string|false|none||留言语音网址|

## POST 标记留言已读

POST /wechat-service/api/device/markLeaveMsgReaded

> Body 请求参数

```json
{
  "id": "string",
  "sessionId": "string",
  "readTime": "string",
  "readerDid": "string"
}
```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|body|body|object| 否 ||none|
|» id|body|string| 是 ||消息ID|
|» sessionId|body|string| 是 ||会话ID|
|» readTime|body|string| 是 ||阅读时间（格式：yyyy-MM-dd HH:mm:ss）|
|» readerDid|body|string| 是 ||当前设备ID|

> 返回示例

> 200 Response

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": "ok"
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|string|true|none||none|

## POST 上传留言/打电话时的拍照

POST /wechat-service/api/device/postCallsPhoto

> Body 请求参数

```yaml
file: file://C:\Users\redclan\Pictures\10-121.png

```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|sessionId|query|string| 否 ||会话ID|
|deviceId|query|string| 否 ||设备ID|
|body|body|object| 否 ||none|
|» file|body|string(binary)| 否 ||none|

> 返回示例

> 200 Response

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": {
        "imgUrl": "https://images-1309522978.file.myqcloud.com/1904817087344279552.png",
        "imgId": "1904817087398805504"
    }
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|integer|true|none||none|
|» msg|string|true|none||none|
|» data|object|true|none||none|
|»» imgUrl|string|true|none||图片访问网址|
|»» imgId|string|true|none||图片在资源库中ID|

## POST 上传留言音频文件

POST /wechat-service/api/device/postCallsAudio

> Body 请求参数

```yaml
file: file://C:\Users\redclan\Videos\countdown.mp3

```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|sessionId|query|string| 否 ||会话ID|
|deviceId|query|string| 否 ||设备ID|
|body|body|object| 否 ||none|
|» file|body|string(binary)| 否 ||none|

> 返回示例

> 200 Response

```json
{
    "code": 0,
    "msg":操作成功,
    "data": {
        "imgUrl": "https://images-1309522978.file.myqcloud.com/1902976942164934656.mp3",
        "imgId": "1902976942173323264"
    })
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|integer|true|none||none|
|» data|object|true|none||none|
|»» imgUrl|string|true|none||音频文件访问网址|
|»» imgId|string|true|none||音频文件在资源库中的ID|

## POST 保存留言

POST /wechat-service/api/device/saveLeaveMsg

文件的存储见【公共方法】-【上传文件】接口

关于留言方向（direction）：

0=儿童向家长留言，（儿童从设备向家长留言，家长在小程序中接收阅读）
左：设备图标 facilityName   右：手机图标，常驻城市（receiverCity）

1=家长向儿童留言（家长在小程序中给儿童留言，儿童在设备中接收阅读）
左：手机图标 常驻城市（fromCity）   右：设备图标  readerDname

4=儿童向手机号留言

5=陌生人向家长（陌生人通过设备向家长留言，家长通过小程序接收阅读）
左：设备图标 facilityName   右：手机图标  常驻城市（receiverCity）

6=家长向陌生人（家长通过小程序留言给陌生人，陌生人通过设备接收阅读）
左：手机图标  常驻城市（fromCity）   右：设备图标 readerDname

3=儿童设备上向儿童留言（儿童通过设备留言，另一儿童设备中接收阅读）
左：设备图标 facilityName    右：设备图标 readerDname

31=儿童手机上向儿童（儿童通过小程序给另一儿童发送留言，另一儿童通过设备接收阅读）
左：手机图标  家长的常驻城市（fromCity）    右：设备图标 readerDname

> Body 请求参数

```json
{
    "fromId": "1891319385092521985",
    "receiverId": "18611611592",
    "relation": "moshengren",
    "direction": "4",
    "duration": 72,
    "deviceId": "ZA2500000001",
    "soundPath": "http://abc.com/123.mp3",
    "imgPath": "http://abc.com/123.jpg",
    "replyId": "174cb87c47ff4d1999eb3f3791b70a01",
    "soundTxt": "non aliqua laboris nostrud"
}
```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|sessionid|header|string| 否 ||会话ID|
|body|body|object| 否 ||none|
|» sessionId|body|string| 是 ||会话ID|
|» fromId|body|string| 是 ||儿童ID|
|» receiverId|body|string| 是 ||接收人ID（如果是陌生人，这里是陌生人11位手机号码）|
|» relation|body|string| 是 ||与儿童关系|
|» direction|body|string| 是 ||留言方向（0=儿童向家长留言，4=向陌生人留言）|
|» duration|body|integer| 是 ||留言时长（单位：秒）|
|» deviceId|body|string| 是 ||留言所在设备ID|
|» soundPath|body|string| 是 ||留言文件网址|
|» soundTxt|body|string| 否 ||留言转文字内容（设备端最好传入）|
|» imgPath|body|string| 是 ||留言时拍照图片网址|
|» replyId|body|string| 是 ||回复目标消息ID（新留言时传值newmsg）|

> 返回示例

> 200 Response

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": {
        "code": "1",
        "msg": "sessionId lost",
        "data": null
    }
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|object|true|none||none|
|»» code|string|true|none||none|
|»» msg|string|true|none||none|
|»» data|null|true|none||none|

## POST 故障上报接口

POST /wechat-service/api/device/postFaultReport

故障上报建议做个异步请求，控制调用频率，一般跟随人机交互事件提交。有大量异常情况下，如有并发，尽量控制在2秒发送1次请求的频率，同一时间（如1分钟内）相同的故障只报1次。

> Body 请求参数

```json
{
  "deviceId": null,
  "etype": null,
  "descs": null,
  "detail": "string"
}
```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|body|body|object| 否 | t_fault_reports|none|
|» deviceId|body|string| 是 ||设备ID|
|» etype|body|string| 是 ||故障类型（硬件=hardware/网络=net/网络电话=sip/语音识别=voice/服务端接口=itfc/其它=other）|
|» descs|body|string| 是 ||故障描述（最大500字节）|
|» detail|body|string| 是 ||故障详情（最大2000字节）|

> 返回示例

> 200 Response

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": "ok"
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|string|true|none||none|

## POST 事件上报接口

POST /wechat-service/api/device/postEventLog

> Body 请求参数

```json
{
  "deviceId": null,
  "etype": null,
  "descs": null,
  "username": "string",
  "babyId": "string",
  "deviceTime": "string"
}
```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|body|body|object| 否 | t_fault_reports|none|
|» deviceId|body|string| 是 ||设备ID|
|» etype|body|string| 是 ||事件类别（重启=reboot/升级=update/上下线=updown/休眠=dormancy/activate=激活）|
|» descs|body|string| 是 ||事件描述（最大500字节）|
|» username|body|string| 是 ||操作人（APP=app/系统=sys/管理员=manager）及ID信息|
|» babyId|body|string| 否 ||最近一个登录的宝宝ID|
|» deviceTime|body|string| 是 ||客户端时间（格式：yyyy-MM-dd HH:mm:ss）|

> 返回示例

> 200 Response

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": "ok"
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|string|true|none||none|

## GET 保持会话

GET /wechat-service/api/device/keepSs

在人脸识别生成会话后，保持宝贝会话为活跃
默认30秒调用1次

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|sid|header|string| 否 ||会话ID（人脸识别时返回）|

> 返回示例

> 200 Response

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": "ok"
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|string|true|none||none|

## GET 结束会话

GET /wechat-service/api/device/closeSs

在宝贝离开后调用

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|sid|header|string| 否 ||会话ID（人脸识别时返回）|

> 返回示例

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": "ok"
}
```

```json
{
    "code": "1",
    "msg": "sessionId lost",
    "data": null
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|string|true|none||none|

## POST 获取宝宝好友列表

POST /wechat-service/api/device/getBabyFriendsList

不分页一次返回

> Body 请求参数

```json
{
    "sessionId": "98aa31ea-4d62-4890-8e4d-423f2faf0510"
}
```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|sessionid|header|string| 否 ||none|
|body|body|object| 否 ||none|
|» sessionId|body|string| 是 ||会话ID|

> 返回示例

> 200 Response

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": [
        {
            "id": "1906004331123965952",
            "userId": "1891311363519676416",
            "friendBabyId": "1905499901341466624",
            "likability": 0,
            "friendBabyName": "小红",
            "gender": 1,
            "contacts": [
                {
                    "mobile": "15655716886",
                    "relation": "mother",
                    "userId": "1905070993278238720",
                    "userName": "用户3455"
                }
            ]
        }
    ]
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|[object]|true|none||none|
|»» id|string|false|none||none|
|»» userId|string|false|none||none|
|»» friendBabyId|string|false|none||好友babyId|
|»» likability|integer|false|none||none|
|»» friendBabyName|string|false|none||好友姓名|
|»» gender|integer|false|none||性别|
|»» contacts|[object]|false|none||好友联系人列表|
|»»» mobile|string|false|none||联系人的手机号码|
|»»» relation|string|false|none||联系人与宝宝关系|
|»»» userId|string|false|none||联系人用户id|
|»»» userName|string|false|none||联系人的姓名|
|»»» answeringType|integer|false|none||优先接听电话类型（0=电话，1=微信）|
|»»» wxOpenid|string|true|none||好友联系人的OpenId|

## POST 加好友-获取宝宝信息

POST /wechat-service/api/device/getBabyInfoByFaceImg

上传人脸照片，返回宝宝信息

此时执行唤醒积分逻辑（首次设备5积分，平常唤醒1积分）

返回值：
{
    "code": "0",
    "msg": "操作成功",
    "data": "JV9DDmqgoTmGEET+Neq0zt7mzuAkov53eWckWujN81RuCb/AGjT21/RLzjZQ0e7GQL94R5OzqTFN5D1pVqszSjZ2SfxZdtajVSVD54pHgkoVLRL6k20/FKiwV/hyaGv+L1HH3eYr5rGBVNhtoGZGwaB6dTnvq4quElj7Nue41Zo4s9AwqPjniB2BChACDpiPEXuOiDRzT7KAPnVr5/gJwkskB/YvkWr7niKluzcdGXEvcfqGLVsCb+15naiTxozydH2udTQAhrJxHelcor8F7D/9t0md9+IlSATvmvkowLM9SXxjibBzVmNUCvHxtJqr62LitmZQYrPX46qUOU1ddA=="
}
data的解密结果：
{"babyName":"邓轩","contacts":[{"mobile":"18611611591","userId":"1891311363519676416"}],"sessionid":"46d7bf7d-e5ad-4dcd-9efc-a7b10575302f"}

> Body 请求参数

```yaml
file://D:\oo\d1.jpg

```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|deviceid|header|string| 否 ||none|
|sessionid|header|string| 否 ||调用此接口返回的sessionId，第一次调用时传空字符不传|
|body|body|object| 否 ||none|
|» file|body|string(binary)| 否 ||none|

> 返回示例

```json
接口返回：
{
    "code": "0",
    "msg": "操作成功",
    "data": "zjQrH76vrfNmJtbxEjmoR/UEeQo3wGg4G/n+aK3PjPoGcdg5mchCUww7xj64hC7ZAnz3lzVrePtbzCwTpr1uBxS2I0V1saFTP3D1oksuK3SItkWWS1eVieCjLOzpaH0ODw7ElgRDj5und65CIA4LwrtsOA05B39y0vSwKqbbFtw="
}
解码后数据：
{"babyId":"1891319385092521985","babyName":"轩轩","gender":"1"}
```

```json
{
    "code": "0",
    "msg": "no user"
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|string|true|none||需要该设备RSA公钥进一步解密|

## POST 加好友-添加另一宝宝为好友

POST /wechat-service/api/device/addFriends

上传人脸照片，返回宝宝信息

此时执行唤醒积分逻辑（首次设备5积分，平常唤醒1积分）

返回值：
{
    "code": "0",
    "msg": "操作成功",
    "data": "JV9DDmqgoTmGEET+Neq0zt7mzuAkov53eWckWujN81RuCb/AGjT21/RLzjZQ0e7GQL94R5OzqTFN5D1pVqszSjZ2SfxZdtajVSVD54pHgkoVLRL6k20/FKiwV/hyaGv+L1HH3eYr5rGBVNhtoGZGwaB6dTnvq4quElj7Nue41Zo4s9AwqPjniB2BChACDpiPEXuOiDRzT7KAPnVr5/gJwkskB/YvkWr7niKluzcdGXEvcfqGLVsCb+15naiTxozydH2udTQAhrJxHelcor8F7D/9t0md9+IlSATvmvkowLM9SXxjibBzVmNUCvHxtJqr62LitmZQYrPX46qUOU1ddA=="
}
data的解密结果：
{"babyName":"邓轩","contacts":[{"mobile":"18611611591","userId":"1891311363519676416"}],"sessionid":"46d7bf7d-e5ad-4dcd-9efc-a7b10575302f"}

> Body 请求参数

```yaml
file://D:\oo\d1.jpg

```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|deviceid|header|string| 否 ||none|
|sessionid|header|string| 否 ||调用此接口返回的sessionId，第一次调用时传空字符不传|
|body|body|object| 否 ||none|
|» babyId|body|string| 否 ||当宝宝好友人脸识别成功，必传|
|» faceImg|body|string(binary)| 否 ||当宝宝好友识别为陌生人，必传|
|» babyName|body|string| 否 ||当宝宝好友识别为陌生人，必传|

> 返回示例

> 200 Response

```json
接口返回：
{
    "code": "0",
    "msg": "操作成功",
    "data": "ok"
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|string|true|none||none|

## GET 获取票据

GET /wechat-service/api/device/getWecooperSnTicket

在发起微信会话时获取SnTicket调用

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|deviceId|query|string| 否 ||none|
|sid|header|string| 否 ||会话ID（人脸识别时返回）|

> 返回示例

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": "ok"
}
```

```json
{
    "code": "1",
    "msg": "sessionId lost",
    "data": null
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|string|true|none||none|

## GET 通知开通服务

GET /wechat-service/api/device/notifyActiveService

当宝贝使用校外版设备时，如果未付费，提醒开通付费服务。
付费逻辑由APP处理，此接口只处理通知(短信+小程序订阅消息)

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|sessionid|header|string| 否 ||会话ID（人脸识别时返回）|

> 返回示例

> 200 Response

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": "ok"
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|string|true|none||none|

## POST 通知视频设备授权

POST /wechat-service/api/device/notifyAuthDeviceGroup

当宝贝通过设备通话遇到家长未视频设备授权时，给家长发送短信及小程序订阅消息
儿童打给自己家长，自己家长应该授权；
儿童打给别人的家长，别人的家长应该授权

> Body 请求参数

```json
test_device_id_1:UJr1/irgHXF0HzZ1moHh0w9a/gsp3478xhZWaJhzsDmd6VZSkh88epCj25t3TJdiqlDf1Q7A431csb4x713eIBu9KfHXD632fs1KwjpKMubNHNSK1xQbSPWWgsyDUt66Vf98zWUTZHFXdkI6oW8WsVRfJ5bivWaweWZhs0PzDfo=
```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|sessionid|header|string| 否 ||会话ID（人脸识别时返回）|
|body|body|object| 否 ||none|
|» targetUserId|body|string| 是 ||打给好友家长时为好友家长的ID，打给自己家长时为自己家长的ID|
|» targetBabyId|body|string| 否 ||好友的Id（打给好友家长时必传，打给自己家长时不传）|

> 返回示例

> 200 Response

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": "ok"
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|string|true|none||none|

# 设备端接口/紧急呼叫

## POST 获取接线员列表和会话ID

POST /wechat-service/api/device/sos/getServiceList

需要每次拨打前获取一下最新接线员列表（列表按接听排序规则返回）
这个接口需要第1个执行，以获取整个紧急呼叫的sessionid（会话ID）

> Body 请求参数

```json
{
  "deviceTime": "string",
  "isIdle": 0,
  "sessionId": "string"
}
```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|deviceid|header|string| 否 ||设备ID|
|aqm-authorization|header|string| 否 ||none|
|body|body|object| 否 ||none|
|» deviceTime|body|string| 是 | 设备时间|格式：yyyy-MM-dd HH:mm:ss|
|» isIdle|body|integer| 是 ||是否只查询空闲座席（1=只返回空闲客服，0=返回所有状态客服）|
|» sessionId|body|string| 是 | 会话ID|第1次请求时，参数sessionId传入空字串，后面紧急呼叫相关API请求时，需要使用此接口返回的sessionid（注意大小写）。|

#### 详细说明

**» sessionId**: 第1次请求时，参数sessionId传入空字串，后面紧急呼叫相关API请求时，需要使用此接口返回的sessionid（注意大小写）。
注意：sessionid是后续步骤的条件。

> 返回示例

> 200 Response

```json
{
  "code": "string",
  "msg": "string",
  "data": {
    "sessionid": "string",
    "sosServiceList": [
      {
        "id": "string",
        "name": "string",
        "mobile": "string",
        "wxOpenId": "string",
        "callStatus": 0,
        "provId": "string",
        "cityId": "string",
        "serviceTime": "string"
      }
    ]
  }
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|string|true|none||none|
|» msg|string|true|none||none|
|» data|object|true|none||none|
|»» sessionid|string|true|none|会话ID|需要记录，后面的上报日志需要用|
|»» sosServiceList|[object]|true|none||none|
|»»» id|string|false|none|客服ID|none|
|»»» name|string|false|none|客服姓名|none|
|»»» mobile|string|false|none|客服手机号码|none|
|»»» wxOpenId|string|false|none|客服微信OpenId|微信呼叫参数|
|»»» callStatus|integer|false|none|在服状态|0=空闲，1=在服|
|»»» provId|string|false|none|省份ID|none|
|»»» cityId|string|false|none|城市ID|none|
|»»» serviceTime|string|false|none|最后一次接线时间|none|

## POST 紧急呼叫接口：上传拍照

POST /wechat-service/api/device/sos/postSosCallsPhoto

注意：sessionid为紧急呼叫专用，与普通正常呼叫不同。

> Body 请求参数

```yaml
file: file://C:\Users\redclan\Pictures\10-121.png

```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|sessionId|query|string| 否 ||会话ID|
|deviceId|query|string| 否 ||设备ID|
|body|body|object| 否 ||none|
|» file|body|string(binary)| 否 ||none|

> 返回示例

> 200 Response

```json
{
    "code": "0",
    "msg": "操作成功",
    "data": {
        "imgUrl": "https://images-1309522978.file.myqcloud.com/1904817087344279552.png",
        "imgId": "1904817087398805504"
    }
}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

状态码 **200**

|名称|类型|必选|约束|中文名|说明|
|---|---|---|---|---|---|
|» code|integer|true|none||none|
|» msg|string|true|none||none|
|» data|object|true|none||none|
|»» imgUrl|string|true|none||图片访问网址|
|»» imgId|string|true|none||图片在资源库中ID|

## POST 紧急呼叫接口：设备上传呼叫开始日志

POST /wechat-service/api/device/sos/postDeviceSosStartLog

取图5张上传完成和获取接线员列表（获得sessionid）后，即刻上传通话开始日志

> Body 请求参数

```json
{
  "sessionid": "string",
  "deviceTime": "string",
  "photos": "string",
  "serviceId": "string"
}
```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|deviceid|header|string| 否 ||设备编号|
|aqm-authorization|header|string| 否 ||none|
|body|body|object| 否 ||none|
|» sessionid|body|string| 是 | 会话ID|从【获取客服列表】接口取得的sessionid|
|» deviceTime|body|string| 是 | 设备时间|格式：yyyy-MM-dd HH:mm:ss|
|» photos|body|string| 是 | 现场图片|5张现场图片URL（用半角分号分隔：";"）|
|» serviceId|body|string| 是 | 接线员ID|none|

#### 详细说明

**» photos**: 5张现场图片URL（用半角分号分隔：";"）
如果接通时，拍照还未完成，可在通话结束接口再传入。

> 返回示例

> 200 Response

```json
{}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

## POST 紧急呼叫接口：设备上传通话开始日志

POST /wechat-service/api/device/sos/postDeviceSosStartChatLog

紧急呼叫，与接线员通话接通时日志

> Body 请求参数

```json
{
  "sessionid": "string",
  "serviceId": "string"
}
```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|deviceid|header|string| 否 ||设备编号|
|aqm-authorization|header|string| 否 ||none|
|body|body|object| 否 ||none|
|» sessionid|body|string| 是 | 会话ID|从【获取客服列表】接口取得的sessionid|
|» serviceId|body|string| 是 | 接线员ID|none|

> 返回示例

> 200 Response

```json
{}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

## POST 紧急呼叫接口：设备上传通话结束日志

POST /wechat-service/api/device/sos/postDeviceSosEndLog

紧急呼叫，设备呼叫接线员通话结束，设备上传通话结束日志

> Body 请求参数

```json
{
  "sessionid": "string",
  "deviceTime": "string",
  "serviceId": "string",
  "photos": "string",
  "serviceLog": "string",
  "timelong": 0
}
```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|deviceid|header|string| 否 ||设备编号|
|aqm-authorization|header|string| 否 ||none|
|body|body|object| 否 ||none|
|» sessionid|body|string| 是 | 会话ID|从【获取客服列表】接口取得的sessionid|
|» deviceTime|body|string| 是 | 设备时间|格式：yyyy-MM-dd HH:mm:ss|
|» serviceId|body|string| 是 | 接线员ID|none|
|» photos|body|string| 是 | 现场图片|5张现场图片URL（用半角分号分隔：";"）|
|» serviceLog|body|string| 是 | 接线日志|客服呼叫接线日志（接线员ID,呼叫时间,接线时间）列表，半角分号分隔。示例：如有3位接线员分别是：A、B、C，ID分别为：0000000001、0000000002、0000000003，A、B均未接通，C接通，则日志应如下：0000000001,2026-01-29 13:19:01,;0000000002,2026-01-29 13:19:30,;0000000002,2026-01-29 13:19:50,2026-01-29 13:20:58|
|» timelong|body|integer| 是 | 通话时长|单位：秒|

> 返回示例

> 200 Response

```json
{}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

## POST 回拨接口：设备上传通话结束日志

POST /wechat-service/api/device/sos/postDeviceSosBackLog

回拨结束，设备上传通话结束日志

> Body 请求参数

```json
{
  "sessionid": "string",
  "deviceTime": "string",
  "serviceId": "string",
  "photos": "string"
}
```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|deviceid|header|string| 否 ||设备编号|
|aqm-authorization|header|string| 否 ||none|
|body|body|object| 否 ||none|
|» sessionid|body|string| 是 | 会话ID|从【获取客服列表】接口取得的sessionid|
|» deviceTime|body|string| 是 | 设备时间|格式：yyyy-MM-dd HH:mm:ss|
|» serviceId|body|string| 是 | 接线员ID|none|
|» photos|body|string| 是 | 现场图片|5张现场图片（用半角分号分隔：";"）|

> 返回示例

> 200 Response

```json
{}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构

## POST 回拨接口：向服务端推送消息

POST /wechat-service/api/device/sos/pushAppMsg

设备启动并完成（摄像头等）初始化后，定时（暂定每60秒）向服务端推送消息（主要为小程序获取pushToken需要）

> Body 请求参数

```json
{
  "deviceId": "string",
  "pushToken": "string",
  "msg": "string"
}
```

### 请求参数

|名称|位置|类型|必选|中文名|说明|
|---|---|---|---|---|---|
|deviceid|header|string| 否 ||设备编号|
|aqm-authorization|header|string| 否 ||none|
|body|body|object| 否 ||none|
|» deviceId|body|string| 是 | 设备ID|none|
|» pushToken|body|string| 是 | pushToken|见：https://developers.weixin.qq.com/doc/oplatform/Miniprogram_Frame/api/cli/device/getPushToken.html|
|» msg|body|string| 否 | 消息|none|

> 返回示例

> 200 Response

```json
{}
```

### 返回结果

|状态码|状态码含义|说明|数据模型|
|---|---|---|---|
|200|[OK](https://tools.ietf.org/html/rfc7231#section-6.3.1)|none|Inline|

### 返回数据结构
