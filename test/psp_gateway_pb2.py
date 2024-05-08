# -*- coding: utf-8 -*-
# Generated by the protocol buffer compiler.  DO NOT EDIT!
# source: psp_gateway.proto
# Protobuf Python Version: 5.26.1
"""Generated protocol buffer code."""
from google.protobuf import descriptor as _descriptor
from google.protobuf import descriptor_pool as _descriptor_pool
from google.protobuf import symbol_database as _symbol_database
from google.protobuf.internal import builder as _builder
# @@protoc_insertion_point(imports)

_sym_db = _symbol_database.Default()




DESCRIPTOR = _descriptor_pool.Default().AddSerializedFile(b'\n\x11psp_gateway.proto\x12\x0bpsp_gateway\"\x84\x01\n\x10TunnelParameters\x12\x10\n\x08mac_addr\x18\x01 \x01(\t\x12\x0f\n\x07ip_addr\x18\x02 \x01(\t\x12\x13\n\x0bpsp_version\x18\x03 \x01(\r\x12\x0b\n\x03spi\x18\x04 \x01(\r\x12\x16\n\x0e\x65ncryption_key\x18\x05 \x01(\x0c\x12\x13\n\x0bvirt_cookie\x18\x06 \x01(\x04\"\xa6\x01\n\x10NewTunnelRequest\x12\x12\n\nrequest_id\x18\x01 \x01(\x04\x12\x1d\n\x15psp_versions_accepted\x18\x02 \x03(\r\x12\x13\n\x0bvirt_src_ip\x18\x03 \x01(\t\x12\x13\n\x0bvirt_dst_ip\x18\x04 \x01(\t\x12\x35\n\x0ereverse_params\x18\x05 \x01(\x0b\x32\x1d.psp_gateway.TunnelParameters\"V\n\x11NewTunnelResponse\x12\x12\n\nrequest_id\x18\x01 \x01(\x04\x12-\n\x06params\x18\x02 \x01(\x0b\x32\x1d.psp_gateway.TunnelParameters\"@\n\x12KeyRotationRequest\x12\x12\n\nrequest_id\x18\x01 \x01(\x04\x12\x16\n\x0eissue_new_keys\x18\x02 \x01(\x08\")\n\x13KeyRotationResponse\x12\x12\n\nrequest_id\x18\x01 \x01(\x04\x32\xbc\x01\n\x0bPSP_Gateway\x12T\n\x13RequestTunnelParams\x12\x1d.psp_gateway.NewTunnelRequest\x1a\x1e.psp_gateway.NewTunnelResponse\x12W\n\x12RequestKeyRotation\x12\x1f.psp_gateway.KeyRotationRequest\x1a .psp_gateway.KeyRotationResponseb\x06proto3')

_globals = globals()
_builder.BuildMessageAndEnumDescriptors(DESCRIPTOR, _globals)
_builder.BuildTopDescriptorsAndMessages(DESCRIPTOR, 'psp_gateway_pb2', _globals)
if not _descriptor._USE_C_DESCRIPTORS:
  DESCRIPTOR._loaded_options = None
  _globals['_TUNNELPARAMETERS']._serialized_start=35
  _globals['_TUNNELPARAMETERS']._serialized_end=167
  _globals['_NEWTUNNELREQUEST']._serialized_start=170
  _globals['_NEWTUNNELREQUEST']._serialized_end=336
  _globals['_NEWTUNNELRESPONSE']._serialized_start=338
  _globals['_NEWTUNNELRESPONSE']._serialized_end=424
  _globals['_KEYROTATIONREQUEST']._serialized_start=426
  _globals['_KEYROTATIONREQUEST']._serialized_end=490
  _globals['_KEYROTATIONRESPONSE']._serialized_start=492
  _globals['_KEYROTATIONRESPONSE']._serialized_end=533
  _globals['_PSP_GATEWAY']._serialized_start=536
  _globals['_PSP_GATEWAY']._serialized_end=724
# @@protoc_insertion_point(module_scope)