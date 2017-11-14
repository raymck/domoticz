local _  = require('lodash')
local utils = require('Utils')

local function HTTPResponce(domoticz, responseData)

    local self = {}

    self.headers = responseData.headers or {}
    self.baseType = domoticz.BASETYPE_HTTP_RESPONSE

    self.data = responseData.data or nil

    self._contentType = _.get(self.headers, {'Content-Type'}, '')

    self.isJSON = false

    self.statusCode = responseData.statusCode

    self.isHTTPResponse = true
    self.isDevice = false
    self.isScene = false
    self.isGroup = false
    self.isTimer = false
    self.isVariable = false

    self.callback = responseData.callback
    self.triggerRule = responseData.callback

    if (string.match(self._contentType, 'application/json') and self.data) then
        local json = utils.fromJSON(self.data)

        if (json) then
            self.isJSON = true
            self.json = json
        end
    end

    return self
end


return HTTPResponce
