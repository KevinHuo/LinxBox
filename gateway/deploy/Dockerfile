# 多阶段构建：运行镜像只携带二进制与必要 CA 证书，不携带 Go 工具链。
FROM golang:1.25 AS build
WORKDIR /src/gateway
COPY gateway/go.mod gateway/go.sum ./
RUN go mod download
COPY contracts /src/contracts
COPY gateway .
ARG TARGETARCH
RUN mkdir -p /out/data && make generate && CGO_ENABLED=0 GOOS=linux GOARCH=${TARGETARCH:-amd64} go build -trimpath -o /out/linxbox-gateway ./cmd/linxbox-gateway

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=build /out/linxbox-gateway /usr/local/bin/linxbox-gateway
# 数据卷挂载 /var/lib/linxbox；配置通过 --config 显式指定。
COPY --from=build --chown=65532:65532 /out/data/ /var/lib/linxbox/
VOLUME ["/var/lib/linxbox"]
EXPOSE 8787
ENTRYPOINT ["/usr/local/bin/linxbox-gateway", "--config", "/etc/linxbox/gateway.json"]
