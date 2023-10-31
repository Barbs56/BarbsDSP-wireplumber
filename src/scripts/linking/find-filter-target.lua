-- WirePlumber
--
-- Copyright © 2023 Collabora Ltd.
--
-- SPDX-License-Identifier: MIT
--
-- Check if the target node is a filter target.

putils = require ("linking-utils")
cutils = require ("common-utils")
futils = require ("filter-utils")
log = Log.open_topic ("s-linking")

function findFilterTarget (si, om)
  local node = si:get_associated_proxy ("node")
  local link_group = node.properties ["node.link-group"]
  local target_id = -1

  -- return nil if session item is not a filter node
  if link_group == nil then
    return nil, false
  end

  -- return nil if filter is not smart
  local direction = cutils.getTargetDirection (si.properties)
  if not futils.is_filter_smart (direction, link_group) then
    return nil, false
  end

  -- get the filter target
  local dont_move = cutils.parseBool (node.properties ["target.dont-move"])
  return futils.get_filter_target (direction, link_group, dont_move), true
end

SimpleEventHook {
  name = "linking/find-filter-target",
  after = "linking/find-defined-target",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "select-target" },
    },
  },
  execute = function (event)
    local source, om, si, si_props, si_flags, target =
        putils:unwrap_select_target_event (event)

    -- bypass the hook if the target is already picked up
    if target then
      return
    end

    local node = si:get_associated_proxy ("node")
    local dont_fallback = cutils.parseBool (node.properties ["target.dont-fallback"])
    local target_picked = false
    local allow_fallback

    log:info (si, string.format ("handling item: %s (%s)",
        tostring (si_props ["node.name"]), tostring (si_props ["node.id"])))

    target, is_smart_filter = findFilterTarget (si, om)

    local can_passthrough, passthrough_compatible
    if target then
      passthrough_compatible, can_passthrough =
          putils.checkPassthroughCompatibility (si, target)
      if putils.canLink (si_props, target) and passthrough_compatible then
        target_picked = true;
      end
    end

    if target_picked and target then
      log:info (si,
        string.format ("... filter target picked: %s (%s), can_passthrough:%s",
          tostring (target.properties ["node.name"]),
          tostring (target.properties ["node.id"]),
          tostring (can_passthrough)))
      si_flags.can_passthrough = can_passthrough
      event:set_data ("target", target)
    elseif is_smart_filter and dont_fallback then
      log:info(si, "... waiting for smart filter defined target as dont-fallback is set")
      event:stop_processing ()
    end
  end
}:register ()
