BarbsDSP Modifications
======================
  - Created new branch called BarbsDSP-wireplumber, made default
  - Editted wireplumber.conf to reduce unnecessary module loading
  - Added profile.conf to wireplumber.conf.d to load a modified version of the main-embedded profile
  - Added bluetooth.conf to wireplumber.conf.d to designate system exclusively as A2DP sink, set codecs to load (AAC, SBC, aptx, aptx HD), enable SBC-XQ, enable AAC VBR with highest quality, and route all streams to PEQ

WirePlumber [Begin orginal developer's unmodified README]
==========================================================

.. image:: https://gitlab.freedesktop.org/pipewire/wireplumber/badges/master/pipeline.svg
   :alt: Pipeline status

.. image:: https://scan.coverity.com/projects/21488/badge.svg
   :alt: Coverity Scan Build Status

.. image:: https://img.shields.io/badge/license-MIT-green
   :alt: License

.. image:: https://img.shields.io/badge/dynamic/json?color=informational&label=tag&query=%24%5B0%5D.name&url=https%3A%2F%2Fgitlab.freedesktop.org%2Fapi%2Fv4%2Fprojects%2F2941%2Frepository%2Ftags
   :alt: Tag

WirePlumber is a modular session / policy manager for
`PipeWire <https://pipewire.org>`_ and a GObject-based high-level library
that wraps PipeWire's API, providing convenience for writing the daemon's
modules as well as external tools for managing PipeWire.

The WirePlumber daemon implements the session & policy management service.
It follows a modular design, having plugins that implement the actual
management functionality.

The WirePlumber Library provides API that allows you to extend the WirePlumber
daemon, to write management or status tools for PipeWire
(apps that don't do actual media streaming) and to write custom session managers
for embedded devices.

Documentation
-------------

The latest version of the documentation is available online
`here <https://pipewire.pages.freedesktop.org/wireplumber/>`_
