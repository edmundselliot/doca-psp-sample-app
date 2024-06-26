# Generated by the gRPC Python protocol compiler plugin. DO NOT EDIT!
"""Client and server classes corresponding to protobuf-defined services."""
import grpc

import psp_gateway_pb2 as psp__gateway__pb2


class PSP_GatewayStub(object):
    """Missing associated documentation comment in .proto file."""

    def __init__(self, channel):
        """Constructor.

        Args:
            channel: A grpc.Channel.
        """
        self.RequestTunnelParams = channel.unary_unary(
                '/psp_gateway.PSP_Gateway/RequestTunnelParams',
                request_serializer=psp__gateway__pb2.NewTunnelRequest.SerializeToString,
                response_deserializer=psp__gateway__pb2.NewTunnelResponse.FromString,
                )
        self.UpdateTunnelParams = channel.unary_unary(
                '/psp_gateway.PSP_Gateway/UpdateTunnelParams',
                request_serializer=psp__gateway__pb2.UpdateTunnelRequest.SerializeToString,
                response_deserializer=psp__gateway__pb2.UpdateTunnelResponse.FromString,
                )
        self.RequestKeyRotation = channel.unary_unary(
                '/psp_gateway.PSP_Gateway/RequestKeyRotation',
                request_serializer=psp__gateway__pb2.KeyRotationRequest.SerializeToString,
                response_deserializer=psp__gateway__pb2.KeyRotationResponse.FromString,
                )


class PSP_GatewayServicer(object):
    """Missing associated documentation comment in .proto file."""

    def RequestTunnelParams(self, request, context):
        """Requests that the recipient allocate a new SPI and encryption key
        so that the initiator can begin sending encrypted traffic.
        """
        context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        context.set_details('Method not implemented!')
        raise NotImplementedError('Method not implemented!')

    def UpdateTunnelParams(self, request, context):
        """Indicates that the SPI+Key previously sent will expire soon, and
        the given SPI+Key in the request should take its place.
        """
        context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        context.set_details('Method not implemented!')
        raise NotImplementedError('Method not implemented!')

    def RequestKeyRotation(self, request, context):
        """Requests that the master key be rotated.
        All active keys become deprecated but continue to work.
        All previously deprecated keys are invalidated.
        """
        context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        context.set_details('Method not implemented!')
        raise NotImplementedError('Method not implemented!')


def add_PSP_GatewayServicer_to_server(servicer, server):
    rpc_method_handlers = {
            'RequestTunnelParams': grpc.unary_unary_rpc_method_handler(
                    servicer.RequestTunnelParams,
                    request_deserializer=psp__gateway__pb2.NewTunnelRequest.FromString,
                    response_serializer=psp__gateway__pb2.NewTunnelResponse.SerializeToString,
            ),
            'UpdateTunnelParams': grpc.unary_unary_rpc_method_handler(
                    servicer.UpdateTunnelParams,
                    request_deserializer=psp__gateway__pb2.UpdateTunnelRequest.FromString,
                    response_serializer=psp__gateway__pb2.UpdateTunnelResponse.SerializeToString,
            ),
            'RequestKeyRotation': grpc.unary_unary_rpc_method_handler(
                    servicer.RequestKeyRotation,
                    request_deserializer=psp__gateway__pb2.KeyRotationRequest.FromString,
                    response_serializer=psp__gateway__pb2.KeyRotationResponse.SerializeToString,
            ),
    }
    generic_handler = grpc.method_handlers_generic_handler(
            'psp_gateway.PSP_Gateway', rpc_method_handlers)
    server.add_generic_rpc_handlers((generic_handler,))


 # This class is part of an EXPERIMENTAL API.
class PSP_Gateway(object):
    """Missing associated documentation comment in .proto file."""

    @staticmethod
    def RequestTunnelParams(request,
            target,
            options=(),
            channel_credentials=None,
            call_credentials=None,
            insecure=False,
            compression=None,
            wait_for_ready=None,
            timeout=None,
            metadata=None):
        return grpc.experimental.unary_unary(request, target, '/psp_gateway.PSP_Gateway/RequestTunnelParams',
            psp__gateway__pb2.NewTunnelRequest.SerializeToString,
            psp__gateway__pb2.NewTunnelResponse.FromString,
            options, channel_credentials,
            insecure, call_credentials, compression, wait_for_ready, timeout, metadata)

    @staticmethod
    def UpdateTunnelParams(request,
            target,
            options=(),
            channel_credentials=None,
            call_credentials=None,
            insecure=False,
            compression=None,
            wait_for_ready=None,
            timeout=None,
            metadata=None):
        return grpc.experimental.unary_unary(request, target, '/psp_gateway.PSP_Gateway/UpdateTunnelParams',
            psp__gateway__pb2.UpdateTunnelRequest.SerializeToString,
            psp__gateway__pb2.UpdateTunnelResponse.FromString,
            options, channel_credentials,
            insecure, call_credentials, compression, wait_for_ready, timeout, metadata)

    @staticmethod
    def RequestKeyRotation(request,
            target,
            options=(),
            channel_credentials=None,
            call_credentials=None,
            insecure=False,
            compression=None,
            wait_for_ready=None,
            timeout=None,
            metadata=None):
        return grpc.experimental.unary_unary(request, target, '/psp_gateway.PSP_Gateway/RequestKeyRotation',
            psp__gateway__pb2.KeyRotationRequest.SerializeToString,
            psp__gateway__pb2.KeyRotationResponse.FromString,
            options, channel_credentials,
            insecure, call_credentials, compression, wait_for_ready, timeout, metadata)
