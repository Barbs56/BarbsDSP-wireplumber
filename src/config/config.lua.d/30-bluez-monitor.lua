-- Bluez monitor config file --

bluez_monitor = {}

bluez_monitor.properties = {
  -- MSBC is not expected to work on all headset + adapter combinations.
  --["bluez5.msbc-support"] = true,
  --["bluez5.sbc-xq-support"] = true,

  -- Enabled headset roles (default: [ hsp_hs hfp_ag ]), this
  -- property only applies to native backend. Currently some headsets
  -- (Sony WH-1000XM3) are not working with both hsp_ag and hfp_ag
  -- enabled, disable either hsp_ag or hfp_ag to work around it.
  --
  -- Supported headset roles: hsp_hs (HSP Headset),
  --                          hsp_ag (HSP Audio Gateway),
  --                          hfp_ag (HFP Audio Gateway)
  --["bluez5.headset-roles"] = "[ hsp_hs hsp_ag hfp_ag ]",

  -- Enabled A2DP codecs (default: all).
  --["bluez5.codecs"] = "[ sbc aac ldac aptx aptx_hd ]",
}

bluez_monitor.rules = {
  -- An array of matches/actions to evaluate.
  {
    -- Rules for matching a device or node. It is an array of
    -- properties that all need to match the regexp. If any of the
    -- matches work, the actions are executed for the object.
    matches = {
      {
        -- This matches all cards.
        { "device.name", "matches", "bluez_card.*" },
      },
    },
    -- Apply properties on the matched object.
    apply_properties = {
      -- ["device.nick"] = "My Device",
    },
  },
  {
    matches = {
      {
        -- Matches all sources.
        { "node.name", "matches", "bluez_input.*" },
      },
      {
        -- Matches all sinks.
        { "node.name", "matches", "bluez_output.*" },
      },
    },
    apply_properties = {
      --["node.nick"] = "My Node",
      --["priority.driver"] = 100,
      --["priority.session"] = 100,
      --["node.pause-on-idle"] = false,
      --["resample.quality"] = 4,
      --["channelmix.normalize"] = false,
      --["channelmix.mix-lfe"] = false,
    },
  },
}

function bluez_monitor.enable()
  load_monitor("bluez", {
    properties = bluez_monitor.properties,
    rules = bluez_monitor.rules,
  })
end
