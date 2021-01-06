## `lws_metrics`

### Introduction

`lws_metrics` records and aggregates **events** at all lws layers.

There are three distinct parts:

 - the architecture inside lws for collecting and aggregating / decimating the
   events and maintaining statistics about them

 - an external handler for forwarding aggregated metrics.  An lws_system ops
   interface to pass on the aggregated metrics to an external backend.  lws
   presents its own public metrics objects and leaves it to the external
   code to have a shim to marry the lws metrics up to whatever is needed in the
   metrics backend

 - a policy for when to emit each type of aggregated information to the external
   handler.  This can be specified in the generic Secure Streams policy.

The external backend interface code may itself make use of lws connectivity apis
including Secure Streams itself, and lws metrics are available on that too.

### `lws_metrics` policy-based reporting

Normally metrics implementations are fixed at build-time and cannot change
without a coordinated reflash of devices along with a change of backend schema.

If the code exists to collect the data, the lws policy and an override api can
be used to modify what is reported to the backend at what rate (or at all).

![policy based metrics](/doc-assets/lws_metrics-policy.png)

### `lws_metrics` decimation

Event information can easily be produced faster than it can be transmitted, or
is useful to record if everything is working.  In the case that things are not
working, then eventually the number of events that are unable to be forwarded
to the backend would overwhelm the local storage.

For that reason, the metrics objects are designed to absorb a large number of
events cheaply by aggregating them, so even extreme situations can be tracked
inbetween dumps to the backend.  There are two approaches:

 - "aggregation": decimate keeping a uint64 mean + sum, along with a max and min
 
 - "histogram": keep a linked-list of different named buckets, with a 64-bit
   counter for the number of times an event in each bucket was observed

A single metric aggregation object has separate "go / no-go" counters, since
most operations can fail, and failing operations act differently.

`lws_metrics` supports decimation by

 - a mean of a 64-bit event metric, separate for go and no-go events
 - counters of go and no-go events
 - a min and max of the metric
 - keeping track of when the sample period started

![metrics decimation](/doc-assets/lws_metrics-decimation.png)

In addition, the policy defines a percentage variance from the mean that
optionally qualifies events to be reported individually.

### `lws_metrics` flags

When the metrics object is created, flags are used to control how it will be
used and consumed.

|Flag|Meaning|
|---|---|
|LWSMTFL_REPORT_OUTLIERS|track outliers and report them internally|
|LWSMTFL_REPORT_OUTLIERS_OOB|report each outlier externally as they happen|
|LWSMTFL_REPORT_INACTIVITY_AT_PERIODIC|explicitly externally report no activity at periodic cb, by default no events in the period is just not reported|
|LWSMTFL_REPORT_MEAN|the mean is interesting for this metric|
|LWSMTFL_REPORT_ONLY_GO|no-go pieces invalid and should be ignored, used for simple counters|
|LWSMTFL_REPORT_DUTY_WALLCLOCK_US|the aggregated sum or mean can be compared to wallclock time| 
|LWSMTFL_REPORT_HIST|object is a histogram (else aggregator)|

### Built-in lws-layer metrics

lws creates and maintains various well-known metrics when you enable build
with cmake `-DLWS_WITH_SYS_METRICS=1`:

#### Aggregation metrics
|metric name|scope|type|meaning|
---|---|---|---|
`cpu.svc`|context|monotonic over time|time spent servicing, outside of event loop wait|
`n.cn.dns`|context|go/no-go mean|duration of blocking libc DNS lookup|
`n.cn.adns`|context|go/no-go mean|duration of SYS_ASYNC_DNS lws DNS lookup|
`n.cn.tcp`|context|go/no-go mean|duration of tcp connection until accept|
`n.cn.tls`|context|go/no-go mean|duration of tls connection until accept|
`n.http.txn`|context|go (2xx)/no-go mean|duration of lws http transaction|
`n.ss.conn`|context|go/no-go mean|duration of Secure Stream transaction|
`n.ss.cliprox.conn`|context|go/no-go mean|time taken for client -> proxy connection|
`vh.[vh-name].rx`|vhost|go/no-go sum|received data on the vhost|
`vh.[vh-name].tx`|vhost|go/no-go sum|transmitted data on the vhost|

#### Histogram metrics
|metric name|scope|type|meaning|
|---|---|---|---|
`n.cn.failures`|context|histogram|Histogram of connection attempt failure reasons|

#### Connection failure histogram buckets
|Bucket name|Meaning|
|---|---|
`tls/invalidca`|Peer certificate CA signature missing or not trusted|
`tls/hostname`|Peer certificate CN or SAN doesn't match the endpoint we asked for|
`tls/notyetvalid`|Peer certificate start date is in the future (time wrong?)|
`tls/expired`|Peer certificate is expiry date is in the past|
`dns/badsrv`|No DNS result because couldn't talk to the server|
`dns/nxdomain`|No DNS result because server says no result|

The `lws-minimal-secure-streams` example is able to report the aggregated
metrics at the end of execution, eg

```
[2021/01/13 11:47:19:9145] U: my_metric_report: cpu.svc: 137.045ms / 884.563ms (15%)
[2021/01/13 11:47:19:9145] U: my_metric_report: n.cn.dns: Go: 4, mean: 3.792ms, min: 2.470ms, max: 5.426ms
[2021/01/13 11:47:19:9145] U: my_metric_report: n.cn.tcp: Go: 4, mean: 40.633ms, min: 17.107ms, max: 94.560ms
[2021/01/13 11:47:19:9145] U: my_metric_report: n.cn.tls: Go: 3, mean: 91.232ms, min: 30.322ms, max: 204.635ms
[2021/01/13 11:47:19:9145] U: my_metric_report: n.http.txn: Go: 4, mean: 63.089ms, min: 20.184ms, max: 125.474ms
[2021/01/13 11:47:19:9145] U: my_metric_report: n.ss.conn: Go: 4, mean: 161.740ms, min: 42.937ms, max: 429.510ms
[2021/01/13 11:47:19:9145] U: my_metric_report: vh._ss_default.rx: Go: (1) 102, NoGo: (1) 0
[2021/01/13 11:47:19:9145] U: my_metric_report: vh.le_via_dst.rx: Go: (22) 28.165Ki
[2021/01/13 11:47:19:9145] U: my_metric_report: vh.le_via_dst.tx: Go: (1) 267
[2021/01/13 11:47:19:9145] U: my_metric_report: vh.api_amazon_com.rx: Go: (1) 1.611Ki, NoGo: (1) 0
[2021/01/13 11:47:19:9145] U: my_metric_report: vh.api_amazon_com.tx: Go: (3) 1.505Ki
```

lws-minimal-secure-stream-testsfail which tests various kinds of connection failure
reports histogram results like this

```
[2021/01/15 13:10:16:0933] U: my_metric_report: n.cn.failures: tot: 36, [ tls/invalidca: 5, tls/expired: 5, tls/hostname: 5, dns/nxdomain: 21 ]
```
